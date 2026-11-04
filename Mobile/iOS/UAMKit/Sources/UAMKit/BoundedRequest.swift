import Foundation

/// URLSession buffers only up to the decoded-body limit, including automatic gzip expansion.
/// The unchecked conformance is confined to lock-protected URLSession delegate state.
final class BoundedRequest: NSObject, URLSessionDataDelegate, @unchecked Sendable
{
    private let transport: BoundedTransport
    private let maximumBytes: Int
    private let lock = NSLock()
    private var continuation: CheckedContinuation<(Data, HTTPURLResponse), any Error>?
    private var task: URLSessionDataTask?
    private var response: HTTPURLResponse?
    private var body = Data()
    private var completed = false
    private var cancelled = false

    init(configuration: URLSessionConfiguration, maximumBytes: Int)
    {
        transport = BoundedTransport(configuration: configuration)
        self.maximumBytes = maximumBytes
    }

    init(transport: BoundedTransport, maximumBytes: Int)
    {
        self.transport = transport
        self.maximumBytes = maximumBytes
    }

    func perform(_ request: URLRequest) async throws -> (Data, HTTPURLResponse)
    {
        try await withTaskCancellationHandler
        {
            try await withCheckedThrowingContinuation { continuation in
                lock.lock()
                if cancelled || completed
                {
                    lock.unlock()
                    continuation.resume(throwing: CompanionError.cancelled)
                    return
                }
                self.continuation = continuation
                guard let task = transport.task(for: request, operation: self) else
                {
                    self.continuation = nil
                    completed = true
                    lock.unlock()
                    continuation.resume(throwing: CompanionError.cancelled)
                    return
                }
                self.task = task
                lock.unlock()
                task.resume()
            }
        } onCancel: { self.cancel() }
    }

    func cancel()
    {
        lock.withLock { cancelled = true }
        finish(.failure(CompanionError.cancelled))
    }

    private func finish(_ result: Result<(Data, HTTPURLResponse), any Error>)
    {
        lock.lock()
        guard !completed else { lock.unlock(); return }
        completed = true
        let continuation = self.continuation
        let task = self.task
        self.continuation = nil
        self.task = nil
        body = Data()
        lock.unlock()
        if let task
        {
            transport.remove(task)
            if case .failure = result { task.cancel() }
        }
        continuation?.resume(with: result)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse, completionHandler: @escaping @Sendable (URLSession.ResponseDisposition) -> Void)
    {
        guard let response = response as? HTTPURLResponse else
        {
            completionHandler(.cancel)
            finish(.failure(CompanionError.invalidResponse("UAM returned a non-HTTP response.")))
            return
        }
        guard response.expectedContentLength <= Int64(maximumBytes) else
        {
            completionHandler(.cancel)
            finish(.failure(CompanionError.responseTooLarge))
            return
        }
        lock.withLock { self.response = response }
        completionHandler(.allow)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data)
    {
        lock.lock()
        guard !completed else { lock.unlock(); return }
        guard data.count <= maximumBytes - body.count else
        {
            lock.unlock()
            finish(.failure(CompanionError.responseTooLarge))
            return
        }
        body.append(data)
        lock.unlock()
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: (any Error)?)
    {
        if let error
        {
            let code = (error as NSError).code
            // Never retain the NSError: userInfo can include a request URL or sensitive server text.
            finish(.failure(code == URLError.cancelled.rawValue ? CompanionError.cancelled : CompanionError.network(code: code)))
            return
        }
        let snapshot = lock.withLock { (body, response) }
        guard let response = snapshot.1 else
        {
            finish(.failure(CompanionError.invalidResponse("UAM returned no HTTP response.")))
            return
        }
        finish(.success((snapshot.0, response)))
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void)
    {
        // Reject all redirects; bearer credentials never move to another origin, scheme or route.
        completionHandler(nil)
        finish(.failure(CompanionError.redirectForbidden))
    }

    func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge, completionHandler: @escaping @Sendable (URLSession.AuthChallengeDisposition, URLCredential?) -> Void)
    {
        // The OS validates the TLS peer and hostname. No unknown-certificate bypass or custom credentials.
        completionHandler(.performDefaultHandling, nil)
    }
}
