import Foundation

/// One connection pool per CompanionClient. The delegate never retains its owner.
final class BoundedTransport: @unchecked Sendable
{
    private let delegate: TransportDelegate
    private let session: URLSession

    init(configuration: URLSessionConfiguration, trust: CompanionTrust? = nil)
    {
        delegate = TransportDelegate(trust: trust)
        session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
    }

    deinit { invalidate() }

    func task(for request: URLRequest, operation: BoundedRequest) -> URLSessionDataTask?
    { delegate.register(request, operation: operation, session: session) }

    func remove(_ task: URLSessionDataTask)
    { delegate.remove(task.taskIdentifier) }

    func invalidate()
    {
        // Stop admission before cancelling operations; late delegate callbacks cannot
        // attach to another operation or resurrect a disconnected connection.
        guard let operations = delegate.stop() else { return }
        operations.forEach { $0.cancel() }
        session.invalidateAndCancel()
    }
}

private final class TransportDelegate: NSObject, URLSessionDataDelegate, @unchecked Sendable
{
    private final class Entry
    {
        // perform() retains its operation while suspended. A weak entry avoids a
        // session/delegate/operation/transport retention cycle, including test owners.
        weak var operation: BoundedRequest?
        init(_ operation: BoundedRequest) { self.operation = operation }
    }
    private let trust: CompanionTrust?
    init(trust: CompanionTrust?) { self.trust = trust }
    private let lock = NSLock()
    private var operations: [Int: Entry] = [:]
    private var stopped = false

    func register(_ request: URLRequest, operation: BoundedRequest, session: URLSession) -> URLSessionDataTask?
    {
        lock.withLock
        {
            guard !stopped else { return nil }
            let task = session.dataTask(with: request)
            operations[task.taskIdentifier] = Entry(operation)
            return task
        }
    }

    func remove(_ id: Int) { _ = lock.withLock { operations.removeValue(forKey: id) } }
    private func operation(_ task: URLSessionTask) -> BoundedRequest?
    { lock.withLock { operations[task.taskIdentifier]?.operation } }

    func stop() -> [BoundedRequest]?
    {
        lock.withLock
        {
            guard !stopped else { return nil }
            stopped = true
            let pending = operations.values.compactMap(\.operation)
            operations.removeAll()
            return pending
        }
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse, completionHandler: @escaping @Sendable (URLSession.ResponseDisposition) -> Void)
    {
        guard let operation = operation(dataTask) else { completionHandler(.cancel); return }
        operation.urlSession(session, dataTask: dataTask, didReceive: response, completionHandler: completionHandler)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data)
    { operation(dataTask)?.urlSession(session, dataTask: dataTask, didReceive: data) }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: (any Error)?)
    { operation(task)?.urlSession(session, task: task, didCompleteWithError: error) }

    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void)
    {
        guard let operation = operation(task) else { completionHandler(nil); return }
        operation.urlSession(session, task: task, willPerformHTTPRedirection: response, newRequest: request, completionHandler: completionHandler)
    }

    func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge, completionHandler: @escaping @Sendable (URLSession.AuthChallengeDisposition, URLCredential?) -> Void)
    {
        guard let trust, challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust else
        { completionHandler(.performDefaultHandling, nil); return }
        let space = challenge.protectionSpace
        guard challenge.previousFailureCount == 0, let serverTrust = space.serverTrust,
              trust.evaluate(serverTrust, host: space.host, port: space.port, protocol: space.protocol) else
        { completionHandler(.cancelAuthenticationChallenge, nil); return }
        completionHandler(.useCredential, URLCredential(trust: serverTrust))
    }
}
