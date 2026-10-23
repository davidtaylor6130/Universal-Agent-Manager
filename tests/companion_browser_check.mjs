import { createServer } from 'node:http';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
// Run after npm --prefix UI-V2 run build. Install Playwright separately; do not use a personal browser profile.
const { chromium, devices } = await import(process.env.UAM_PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(process.argv[2] || 'UI-V2/dist');
const output = path.resolve('.UAM/AINotes/mobile-companion/browser-evidence');
await mkdir(output, { recursive: true });
const chats = ['a','b','c'].map((id,i) => ({id, title: `Chat ${id.toUpperCase()}`, providerId:'codex-cli', folderId:'work', createdAt:'2026-10-03T10:00:00Z', updatedAt:'2026-10-03T11:00:00Z', messageCount:1, workspaceDirectory:'/tmp/uam-mobile-fixture', acpSession: {running:true, processing:i===0, readySinceLastSelect:i===1, sessionId:id, providerId:'codex-cli'}}));
let revision = 1; const requests = [];
const state = () => ({bootId:'browser-fixture',stateRevision:revision,selectedChatId:'a',folders:[{id:'work',title:'Test workspace',directory:'/tmp/uam-mobile-fixture',collapsed:false}], chats, providers:[{id:'codex-cli',name:'Codex',shortName:'Codex',supportsStructured:true,supportsCli:true,structuredProtocol:'acp'}],settings:{theme:'focus',activeProviderId:'codex-cli'}});
const types = {'.html':'text/html','.js':'text/javascript','.css':'text/css','.png':'image/png','.svg':'image/svg+xml','.woff2':'font/woff2','.webmanifest':'application/manifest+json'};
const server = createServer(async (req,res) => {
 try {
  if (req.method==='POST' && req.url==='/api') {
   let raw=''; for await (const part of req) raw+=part;
   const body=JSON.parse(raw); requests.push(body);
   let result={};
   if(body.action==='getInitialState') result=state();
   if(body.action==='getChatMessages') {
    await new Promise(resolve=>setTimeout(resolve,body.payload.chatId==='a'?400:40));
    result={chatId:body.payload.chatId,messages:[{role:'assistant',content:`Transcript ${body.payload.chatId.toUpperCase()}`,timestamp:'2026-10-03T11:00:00Z'}],startIndex:0,totalCount:1,messagesDigest:'fixture-'+body.payload.chatId};
   }
   if(body.action==='listUamAgents') result={agents:[]};
   if(body.action==='discoverProviderModels') result={models:[]};
   res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(result));return;
  }
  const pathname=new URL(req.url,'http://localhost').pathname;
  const file=path.join(root,pathname==='/companion'?'index.html':pathname.slice(1));
  const bytes=await readFile(file);res.writeHead(200,{'Content-Type':types[path.extname(file)] || 'application/octet-stream'});res.end(bytes);
 }catch(error){res.writeHead(404);res.end('Not found');}
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const url=`http://127.0.0.1:${server.address().port}/companion`;
const browser=await chromium.launch({headless:true});const results=[];
try {
 for (const device of ['iPhone 13','Pixel 7']) {
  const context=await browser.newContext({...devices[device]});
  await context.addInitScript(()=>{
    localStorage.setItem('uam-companion-token','isolated-fixture');
    window.uamNotificationCalls=[];
    const show=ServiceWorkerRegistration.prototype.showNotification;
    ServiceWorkerRegistration.prototype.showNotification=async function(title,options){
      const call={title,tag:options.tag,delivered:false};window.uamNotificationCalls.push(call);
      await show.call(this,title,options);call.delivered=true;
    };
  });
  const page=await context.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto(url);
  await page.locator('.uam-companion-section').first().waitFor();
  assert.equal(await page.locator('.uam-companion-activity-row').count(),2);
  // Switch again before Chat A's deliberately slow transcript response arrives.
  await page.locator('[data-session-id="a"]').first().click();
  await page.getByRole('button',{name:'Chats',exact:true}).click();
  await page.locator('[data-session-id="b"]').last().click();
  await page.getByText('Transcript B',{exact:true}).waitFor();
  await page.waitForTimeout(500);
  assert.equal(await page.locator('.uam-companion-header strong').innerText(),'Chat B');
  await page.getByRole('button',{name:'Chats',exact:true}).click();
  await page.locator('[data-session-id="c"]').last().focus();
  await page.keyboard.press('Enter');
  await page.getByText('Transcript C',{exact:true}).waitFor();
  for(const id of ['a','b','c','b','a']) {
   await page.getByRole('button',{name:'Chats',exact:true}).click();
   const row=page.locator(`[data-session-id="${id}"]`).last();await row.click();
   await page.locator('.uam-companion-header strong').filter({hasText:`Chat ${id.toUpperCase()}`}).waitFor();
   await page.getByText(`Transcript ${id.toUpperCase()}`,{exact:true}).waitFor();
   const overlap=await page.evaluate(()=>{const composer=document.querySelector('.uam-composer-textarea').getBoundingClientRect();const plus=document.querySelector('.uam-companion-new-chat').getBoundingClientRect();return composer.bottom>plus.top && composer.top<plus.bottom && composer.left<plus.right && composer.right>plus.left});
   assert.equal(overlap,false,'composer overlaps new chat');
  }
  await page.screenshot({path:path.join(output,device.replaceAll(' ','-')+'-chat.png')});
  // Keyboard-sized viewport keeps both composer and navigation inside the layout.
  await page.setViewportSize({width:devices[device].viewport.width,height:400});
  assert.equal(await page.evaluate(()=>document.querySelector('.uam-composer-textarea').getBoundingClientRect().bottom<=document.querySelector('.uam-companion-nav').getBoundingClientRect().top),true);
  await page.getByRole('button',{name:'Chats',exact:true}).click();
  await page.getByRole('button',{name:'Settings',exact:true}).click();
  await page.getByRole('combobox',{name:'Theme',exact:true}).click();
  await page.getByRole('option',{name:'Midnight',exact:true}).click();
  await page.getByRole('button',{name:'Refresh chats and activity',exact:true}).click();
  assert.match(await page.getByRole('combobox',{name:'Theme',exact:true}).innerText(),/Midnight/);
  assert.equal(await page.evaluate(()=>document.documentElement.dataset.theme),'midnight');
  await context.grantPermissions(['notifications'],{origin:new URL(url).origin});
  await page.getByRole('button',{name:'Enable browser notifications',exact:true}).click();
  await page.waitForFunction(()=>document.body.innerText.includes('Disable browser notifications') || document.body.innerText.includes('Notifications are blocked.'));
  const nativePermission=await page.evaluate(()=>Notification.permission);
  if(nativePermission==='granted') await page.getByRole('button',{name:'Disable browser notifications',exact:true}).waitFor();
  else await page.getByText('Notifications are blocked. Allow them in your browser settings.',{exact:true}).waitFor();
  await page.getByLabel('Input alerts',{exact:true}).uncheck();
  await page.getByLabel('Input alerts',{exact:true}).check();
  chats[2].acpSession={...chats[2].acpSession,attentionKind:'question',lifecycleState:'waitingUserInput',processing:true};revision++;
  await page.getByRole('button',{name:'Refresh chats and activity',exact:true}).click();
  assert.equal(await page.locator('.uam-companion-attention').count(),0);
  await page.screenshot({path:path.join(output,device.replaceAll(' ','-')+'-settings.png')});
  await page.getByRole('button',{name:'Activity',exact:true}).click();
  await page.locator('.uam-companion-activity-row[data-session-id="c"]').click();
  await page.getByText('Transcript C',{exact:true}).waitFor();
  assert.deepEqual(errors,[]);
  results.push({device,viewport:devices[device].viewport,checks:'current activity, repeated chat switching, keyboard selection, delayed transcript, composer overlap, keyboard height, theme persistence, input status without banner, service-worker permission handling',pageErrors:errors,nativeNotificationPermission:nativePermission,nativeDeliveryVerified:nativePermission==='granted'});
  chats[2].acpSession={...chats[2].acpSession,attentionKind:null,lifecycleState:'idle',processing:false};revision++;
  await context.close();
 }
 assert.equal(requests.some(r=>r.action==='selectSession'||r.action==='setTheme'),false,'phone changed desktop settings');
 await writeFile(path.join(output,'results.json'),JSON.stringify(results,null,2));console.log(JSON.stringify(results,null,2));
} finally {await browser.close();server.close();}
