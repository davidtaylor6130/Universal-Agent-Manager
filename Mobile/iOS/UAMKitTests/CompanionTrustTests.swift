import Foundation
import Security
import XCTest
@testable import UAMKit

final class CompanionTrustTests: XCTestCase
{
    private func trust(at date: Date = Date(timeIntervalSince1970: 1792195200), certificate: String = TrustFixture.leaf) throws -> SecTrust
    {
        let leaf = try XCTUnwrap(SecCertificateCreateWithData(nil, try XCTUnwrap(Data(base64Encoded: certificate)) as CFData))
        var result: SecTrust?
        XCTAssertEqual(SecTrustCreateWithCertificates([leaf] as CFArray, SecPolicyCreateSSL(true, "paired.example" as CFString), &result), errSecSuccess)
        let trust = try XCTUnwrap(result)
        XCTAssertEqual(SecTrustSetVerifyDate(trust, date as CFDate), errSecSuccess)
        XCTAssertEqual(SecTrustSetNetworkFetchAllowed(trust, false), errSecSuccess)
        return trust
    }

    func testChosenRootValidatesMatchingHostnameChainAndDates() throws
    {
        let policy = try CompanionTrust(baseURL: URL(string: "https://paired.example")!, certificate: TrustFixture.root)
        let chain = try trust()
        let accepted = policy.evaluate(chain, host: "paired.example", port: 443, protocol: "https")
        var error: CFError?
        if !accepted { _ = SecTrustEvaluateWithError(chain, &error) }
        XCTAssertTrue(accepted, String(describing: error))
    }

    func testOtherHostnameFailsEvenWithChosenRoot() throws
    {
        let policy = try CompanionTrust(baseURL: URL(string: "https://other.example")!, certificate: TrustFixture.root)
        XCTAssertFalse(policy.evaluate(try trust(), host: "other.example", port: 443, protocol: "https"))
    }

    func testUnrelatedRootCannotValidateThePresentedChain() throws
    {
        let policy = try CompanionTrust(baseURL: URL(string: "https://paired.example")!, certificate: TrustFixture.otherRoot)
        XCTAssertFalse(policy.evaluate(try trust(), host: "paired.example", port: 443, protocol: "https"))
    }

    func testExpiredAndNotYetValidLeafAreRefused() throws
    {
        let policy = try CompanionTrust(baseURL: URL(string: "https://paired.example")!, certificate: TrustFixture.root)
        for date in [Date(timeIntervalSince1970: 1262304000), Date(timeIntervalSince1970: 2524608000)]
        { XCTAssertFalse(policy.evaluate(try trust(at: date), host: "paired.example", port: 443, protocol: "https")) }
    }

    func testAnchorIsBoundToExactHTTPSHostAndPort() throws
    {
        let policy = try CompanionTrust(baseURL: URL(string: "https://PAIRED.example:8443/companion")!, certificate: TrustFixture.root)
        XCTAssertTrue(policy.matches(host: "paired.example", port: 8443, protocol: "https"))
        XCTAssertFalse(policy.matches(host: "other.example", port: 8443, protocol: "https"))
        XCTAssertFalse(policy.matches(host: "paired.example", port: 443, protocol: "https"))
        XCTAssertFalse(policy.matches(host: "paired.example", port: 8443, protocol: "http"))
    }

    func testMalformedOversizedOrNonCertificateAnchorsAreRejectedBeforeNetwork()
    {
        for value in ["!invalid", "", Data("not a certificate".utf8).base64EncodedString(), Data(repeating: 1, count: 8193).base64EncodedString(), TrustFixture.root + "\n"]
        {
            XCTAssertThrowsError(try CompanionClient(baseURL: URL(string: "https://paired.example")!, token: "fixture", pairingCertificate: value))
            { error in XCTAssertEqual(error as? CompanionError, .invalidPairingCertificate) }
        }
    }
    func testOverlongServerCertificateValidityIsStillRefused() throws
    {
        let policy = try CompanionTrust(baseURL: URL(string: "https://paired.example")!, certificate: TrustFixture.root)
        XCTAssertFalse(policy.evaluate(try trust(certificate: TrustFixture.longLeaf), host: "paired.example", port: 443, protocol: "https"))
    }

}

private enum TrustFixture
{
    static let root = "MIIC+jCCAeKgAwIBAgIUJvOfWpi2eJuY/0OyPGrmfxEyKsEwDQYJKoZIhvcNAQELBQAwIzEhMB8GA1UEAwwYRGlzcG9zYWJsZSBVQU0gVGVzdCBSb290MB4XDTIwMDEwMTAwMDAwMFoXDTQwMDEwMTAwMDAwMFowIzEhMB8GA1UEAwwYRGlzcG9zYWJsZSBVQU0gVGVzdCBSb290MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEArSbv9d80hcK/TE5FMEsg3BLRg0BsGZ+2QAw3jETfMF8mHshe9aQlKGJ+JrxLmasXJKQBOD6hybZikYatTKLus5Ib1pWG5S/Po4ROkhTjd2+2icvdJyCJ7cnlR+A1X06N1tw0b/7l/lrA2h9f4qDzwMAI/QoceJpjiuLS/1SO/84YiIEcHQO7Hgn3FitotMwzcV57y2DLulLAALnCQJ1/tnSsMlCspQnGGqiJqVwQKG7B2wAPUH7EPEyyJcPPGV/V91d0zggTdjRYYCX08DqEL6VgA3X4y6GteDHTlwOu17y5NQbBZ2WW0Iejzyy+BjG7g6tQkqpC8wbpVcEry+AacQIDAQABoyYwJDASBgNVHRMBAf8ECDAGAQH/AgEBMA4GA1UdDwEB/wQEAwIBBjANBgkqhkiG9w0BAQsFAAOCAQEAkXeMIh1pMAzk7HMEMT3JPbPB2iWlSV9jee3HQXOuP3yYUURerLwVNm6ihW+qgMYrjE8rxRFy+bxFyq4C6sndrJw+3p8T54zXwKpJS74plD4/XN3bGKXNqhrvKQU6pIFw/eJZv3/2A+eg/7rp83ALI7Zz/5NMlsoNWshzSAeFAcN2kat7qvamqUenZZtliVH8qBmOH0evWpc/HkOfEM/7A46BfCkvxnnaGuU1vqKk6jBDXS7dd82IQCjfwwPGdfaEWeu1YyqlU7lnNnctW3Ip1hMliM9TyRGJ+K7t4RDLNMY+zxp0qR7EZso+ku+VWkiKCj55zwuJykJM1hmtVhPF8g=="
    static let otherRoot = "MIIC+DCCAeCgAwIBAgIULt9nbvmgl2+wY69qs2doCop0R1cwDQYJKoZIhvcNAQELBQAwIjEgMB4GA1UEAwwXVW5yZWxhdGVkIFVBTSBUZXN0IFJvb3QwHhcNMjAwMTAxMDAwMDAwWhcNNDAwMTAxMDAwMDAwWjAiMSAwHgYDVQQDDBdVbnJlbGF0ZWQgVUFNIFRlc3QgUm9vdDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAJjxqMu9fZDtlUuw6JT3nLs+bPFskxxCyg1VPwc/Yh3AJkm7Oge1xGjIZwPMpKStzYrNwGG/nKYgvtr08JL68njyqzeRJE5fNqR4rWRvVYJinfcD9VtOegaaiyTKBXonItYmkHSxz0fu6/dAVSOiaqHhKr24vI7Jp/sq4uhEXSdqzt0C57NwRp9FmKTyJa6uIDVEePvyy3HCf4/V1q0Jb0ETC+O9Y8PZ3e15lbVE+K9BeDDShFbuiNw628/LPdl2E/f/Qeivdnf9D6VLlFwrUwwGtULCyQyYoajFxumrhjwa1f3FFm7a3uDtGk0yD6WtCCR5d2gcNcIuir+5L8AGStMCAwEAAaMmMCQwEgYDVR0TAQH/BAgwBgEB/wIBATAOBgNVHQ8BAf8EBAMCAQYwDQYJKoZIhvcNAQELBQADggEBADXOtKYsPIDLagZm3KT2k3o1/oCz9gTHI3fKOOnpJyXc8CpqjU1YHGVl+F1ksC/I/Be6li6hvdhFcektFiWHGoohxihzxe+EM2nFcmp2/MP8ahPtUzuLDfaJl+KT0BzHnty358m0byeE/6stxsNuelnoV8jWBjoPmsKvSFvEI4E1ekPIo0KD76Ld5c+xxmazIEkYmgm7jQRBxC0iuYvLs2yvps8yb6ssvX1pbhkVUEUsqwjZ1i6thveytL5jS8ZqiVfufor1+ZSLRKA0KmFQeRRFdBqUdUTQ7d7SORsYpziqj+vnQWtkC4F6movPCwJojb19t+lInnTCs5ze6AvJbzg="
    static let leaf = "MIIDGjCCAgKgAwIBAgIUYmRvMiVt0Jd/ceJBaXdL9BrRdK0wDQYJKoZIhvcNAQELBQAwIzEhMB8GA1UEAwwYRGlzcG9zYWJsZSBVQU0gVGVzdCBSb290MB4XDTI2MDEwMTAwMDAwMFoXDTI2MTIzMTAwMDAwMFowGTEXMBUGA1UEAwwOcGFpcmVkLmV4YW1wbGUwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDYcyGuBUwlaD6eM4NjB/Uc+0uS9lDC861nhQOJHmtSkz3bYbauZXt2dfyeja6Y0NkgV8EhoSMwn6vfCzOMnAUCUUhYda9xVrKofWbII4j9vuiF+OQ658p9CQtBLEvXhvw7wEaPOotKZFZWjxv2uB/VetXCd4Qf7D8N3yJQzyPBCkMqPmSKI4MFN6JyfErgJNHzBE+NA6oBGmtJkEC5qTT60Rmi56qpg6CGu8WDYeaU8gf7dW9oIpBB1TuvTo996/v0YHUcIlZzmjh9u28BWU01RDQ8ze8jL0DdDAI60NoFBnuIdjr6BH38X8Cyfltl3cODpVuUCiIs/sCPYgGtiFoNAgMBAAGjUDBOMAwGA1UdEwEB/wQCMAAwGQYDVR0RBBIwEIIOcGFpcmVkLmV4YW1wbGUwEwYDVR0lBAwwCgYIKwYBBQUHAwEwDgYDVR0PAQH/BAQDAgWgMA0GCSqGSIb3DQEBCwUAA4IBAQB93o3D3SIuPlXBuKMXehtRH9G32aYLSTVKsxgLIO+/wYgJSuCZ5ghO6iTGSHKdJ0m9BBZi1x6lUQMYluQRAbZffDIRQAmC16/u5yOgWNsKSjf/+1VTzwQTDFyh2dHvIMZCRTinw2Lp++h54jy7cT9jgD4bcM+Wm8TEcOdgOr8c8gwFE/Zcv8DJYlLJ8XcYQdkzv9Ymk+breP9RF5y1y9WMH3i6DfIpUYre7bvbkuumt7Gpec+22hUsiUrRp1iQoLdEtRF/zRv+uI9v/mDzMXIEkuhqiGHeZ16duoT2Lqmgatvirl7BVOlbWqrkcD0vAZbkT6fyB4z/5cCxoQQ5U1pW"
    static let longLeaf = "MIIDGjCCAgKgAwIBAgIUZf0Eyf4HSDrrXkQrsxzJ/S2u0OYwDQYJKoZIhvcNAQELBQAwIzEhMB8GA1UEAwwYRGlzcG9zYWJsZSBVQU0gVGVzdCBSb290MB4XDTIwMDEwMTAwMDAwMFoXDTQwMDEwMTAwMDAwMFowGTEXMBUGA1UEAwwOcGFpcmVkLmV4YW1wbGUwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC2JluS8p8SySVDjzRP+iC8EBiDK/7PRpUzue4HefZrgbYBni96R0TsOq8ywC9SAvNh4FNl9+JqX8bxLekt5ijmwnQ4KsZebLBs/iLFSJGUtQurNNoHAIbuh8y9Rpu0cIfcu8564H2MepGylrV2a1VyYTaF2BiDXNS1Ht+ONXyyNE0MNyF7QUGzdz3AbuC9q8kyQlzdmPkzffvlP3/+5aSwtJtXLzoLeId9sLGfViJ5GaMpZbWmUstxvqwBlt9K0+oUUI3O0ht7NrNBfMuEMSs0xwass1PD07IGhS2+QUkDJ3dwmsMNbuPTwpXi7thCKQ+Ak7LTdrVGC7a5ItLISrA5AgMBAAGjUDBOMAwGA1UdEwEB/wQCMAAwGQYDVR0RBBIwEIIOcGFpcmVkLmV4YW1wbGUwEwYDVR0lBAwwCgYIKwYBBQUHAwEwDgYDVR0PAQH/BAQDAgWgMA0GCSqGSIb3DQEBCwUAA4IBAQByvDB2ZCatdNCOlFOrzmicvHKpZvQnwKqHH49fc3OZjgsXpX1fDAVwS7a8/2KJ1jdO8rPoKGH7sbmnbCBSSHI+Ouf9v2HM8mh+59Wv72PgPY5qtZKFWXgCSuQpPNZWoc26o3dzc8ifgIytTLgdpXVNhpaedA+Pt74lHxbCKHlBBv+XaQGbh6nFwQH7yulADfWBOZ1QAQa4EtB6FwRlb2b8+qD3owcPrc05hOdeNThzDR9xTSxH4eKA1dYyIat/PrRHl/iqD8bu8KVz/iS1gYEeBumsoDPg6TDEGiCfVBE4pfS5CpbELzZStBl1Pn1VXXoe7R13M80ct/8sizc0yc/x"
}
