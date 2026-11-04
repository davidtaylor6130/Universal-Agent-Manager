#if os(macOS)
import Foundation
import XCTest
@testable import UAMKit

/// Real sockets catch connection-pool regressions that URLProtocol cannot observe.
final class KeepAliveTests: XCTestCase, @unchecked Sendable
{
    private func server() throws -> (Process, URL)
    {
        guard FileManager.default.isExecutableFile(atPath: "/usr/bin/python3") else
        { throw XCTSkip("Local HTTP/1.1 fixture requires macOS developer Python.") }
        let process = Process(), output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        process.arguments = ["-u", "-c", #"""
import base64, http.server, json, socket
class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    connections = 0
    requests = 0
    chunks = 0
    commits = 0
    offset = 0
    total = 0
    def get_request(self):
        conn, addr = super().get_request()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.connections += 1
        return conn, addr
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args): pass
    def do_POST(self):
        value = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        action, payload = value['action'], value.get('payload', {})
        self.server.requests += 1
        if action == 'getCompanionCapabilities':
            data = {'protocolVersion': 1, 'features': {'requestChunks': True}, 'limits': {'maxRequestBytes': 33554432, 'requestChunkBytes': 32768}}
        elif action == 'beginCompanionRequestTransfer':
            self.server.total, self.server.offset = payload['totalBytes'], 0
            data = {'id': 'fixture', 'totalBytes': self.server.total, 'nextOffset': 0, 'chunkBytes': 32768}
        elif action == 'appendCompanionRequestChunk':
            assert payload['offset'] == self.server.offset
            self.server.offset += len(base64.b64decode(payload['base64'], validate=True))
            self.server.chunks += 1
            data = {'id': 'fixture', 'nextOffset': self.server.offset}
        else:
            if action == 'commitCompanionRequestTransfer':
                assert self.server.offset == self.server.total
                self.server.commits += 1
            data = {'connections': self.server.connections, 'requests': self.server.requests, 'chunks': self.server.chunks, 'commits': self.server.commits}
        body = json.dumps({'ok': True, 'data': data}, separators=(',', ':')).encode()
        headers = ('HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n' % len(body)).encode()
        self.wfile.write(headers + body)
server = Server(('127.0.0.1', 0), Handler)
print(server.server_address[1], flush=True)
server.serve_forever()
"""#]
        process.standardOutput = output
        try process.run()
        addTeardownBlock
        {
            if process.isRunning { process.terminate(); process.waitUntilExit() }
            try? output.fileHandleForReading.close()
        }
        var line = Data()
        while let byte = try output.fileHandleForReading.read(upToCount: 1), !byte.isEmpty
        {
            if byte == Data([10]) { break }
            line.append(byte)
            guard line.count < 10 else { throw URLError(.cannotConnectToHost) }
        }
        let port = try XCTUnwrap(String(data: line, encoding: .utf8).flatMap(Int.init))
        return (process, URL(string: "http://127.0.0.1:\(port)")!)
    }

    func testSequentialBoundedRequestsReuseOneRealHTTP11Connection() async throws
    {
        let (_, endpoint) = try server()
        let transport = BoundedTransport(configuration: .ephemeral)
        defer { transport.invalidate() }
        var last: JSONValue = .null
        for _ in 0..<50
        {
            var request = URLRequest(url: endpoint.appendingPathComponent("api"))
            request.httpMethod = "POST"; request.httpBody = Data(#"{"action":"fixture"}"#.utf8)
            let (body, _) = try await BoundedRequest(transport: transport, maximumBytes: 1024).perform(request)
            last = try CompanionProtocol.unwrap(CompanionProtocol.decode(body))
        }
        XCTAssertEqual(last.int("connections"), 1)
        XCTAssertEqual(last.int("requests"), 50)
    }

    #if DEBUG
    func testTwentyMiBAttachmentUploadReusesOneConnectionAndCommitsOnce() async throws
    {
        let (_, endpoint) = try server()
        let client = try CompanionClient(baseURL: endpoint, token: "disposable-fixture-only", allowLoopbackHTTP: true)
        let base64 = Data(repeating: 0x61, count: 20 * 1024 * 1024).base64EncodedString()
        let result = try await client.request(action: "stageChatAttachments", payload: ["chatId": "fixture", "items": [["name": "fixture.txt", "mimeType": "text/plain", "dataBase64": .string(base64)]]])
        XCTAssertEqual(result.int("connections"), 1)
        XCTAssertEqual(result.int("requests"), 857)
        XCTAssertEqual(result.int("chunks"), 854)
        XCTAssertEqual(result.int("commits"), 1)
        await client.disconnect()
    }
    #endif
}
#endif
