import Foundation

/// Lossless for JSON integers, including provider identifiers above Double's exact range.
public enum JSONValue: Sendable, Equatable, Codable
{
    case null
    case bool(Bool)
    case integer(Int64)
    case unsignedInteger(UInt64)
    case number(Double)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])

    public init(from decoder: any Decoder) throws
    {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() { self = .null }
        else if let value = try? container.decode(Bool.self) { self = .bool(value) }
        else if let value = try? container.decode(Int64.self) { self = .integer(value) }
        else if let value = try? container.decode(UInt64.self) { self = .unsignedInteger(value) }
        else if let value = try? container.decode(Double.self), value.isFinite { self = .number(value) }
        else if let value = try? container.decode(String.self) { self = .string(value) }
        else if let value = try? container.decode([JSONValue].self) { self = .array(value) }
        else { self = .object(try container.decode([String: JSONValue].self)) }
    }

    public func encode(to encoder: any Encoder) throws
    {
        var container = encoder.singleValueContainer()
        switch self
        {
        case .null: try container.encodeNil()
        case .bool(let value): try container.encode(value)
        case .integer(let value): try container.encode(value)
        case .unsignedInteger(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .string(let value): try container.encode(value)
        case .array(let value): try container.encode(value)
        case .object(let value): try container.encode(value)
        }
    }

    public var objectValue: [String: JSONValue]? { if case .object(let value) = self { value } else { nil } }
    public var arrayValue: [JSONValue]? { if case .array(let value) = self { value } else { nil } }
    public var stringValue: String? { if case .string(let value) = self { value } else { nil } }
    public var boolValue: Bool? { if case .bool(let value) = self { value } else { nil } }
    public var intValue: Int?
    {
        switch self
        {
        case .integer(let value): return Int(exactly: value)
        case .unsignedInteger(let value): return Int(exactly: value)
        case .number(let value): return Int(exactly: value)
        default: return nil
        }
    }
    public var doubleValue: Double?
    {
        switch self
        {
        case .integer(let value): return Double(value)
        case .unsignedInteger(let value): return Double(value)
        case .number(let value): return value
        default: return nil
        }
    }
    public subscript(_ key: String) -> JSONValue?
    {
        get { objectValue?[key] }
        set
        {
            guard case .object(var object) = self else { return }
            object[key] = newValue
            self = .object(object)
        }
    }
    public subscript(_ index: Int) -> JSONValue?
    {
        guard let array = arrayValue, array.indices.contains(index) else { return nil }
        return array[index]
    }
    public func string(_ key: String) -> String? { self[key]?.stringValue }
    public func bool(_ key: String) -> Bool? { self[key]?.boolValue }
    public func int(_ key: String) -> Int? { self[key]?.intValue }
    public func array(_ key: String) -> [JSONValue]? { self[key]?.arrayValue }
    public func object(_ key: String) -> [String: JSONValue]? { self[key]?.objectValue }
    public func decode<T: Decodable>(_ type: T.Type = T.self) throws -> T
    {
        try JSONDecoder().decode(type, from: JSONEncoder().encode(self))
    }
}

extension JSONValue: ExpressibleByStringLiteral, ExpressibleByBooleanLiteral, ExpressibleByIntegerLiteral, ExpressibleByFloatLiteral, ExpressibleByNilLiteral, ExpressibleByArrayLiteral, ExpressibleByDictionaryLiteral
{
    public init(stringLiteral value: String) { self = .string(value) }
    public init(booleanLiteral value: Bool) { self = .bool(value) }
    public init(integerLiteral value: Int64) { self = .integer(value) }
    public init(floatLiteral value: Double) { self = .number(value) }
    public init(nilLiteral: ()) { self = .null }
    public init(arrayLiteral elements: JSONValue...) { self = .array(elements) }
    public init(dictionaryLiteral elements: (String, JSONValue)...) { self = .object(Dictionary(elements, uniquingKeysWith: { _, new in new })) }
}
