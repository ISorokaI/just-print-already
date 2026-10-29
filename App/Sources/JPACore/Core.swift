import Foundation

/// One row of drivers/models.tsv.
public struct Driver: Equatable, Sendable {
    public let match, name, manufacturer, filter: String
    public let model: Int
    public let tested: Bool
}

public struct Printer: Identifiable, Sendable {
    public let uri: String
    public let model: String  // as the printer reports it over USB
    public let driver: Driver?
    public var queue: String?  // our CUPS queue for this URI, if already set up
    public var id: String { uri }

    public init(uri: String, model: String, driver: Driver?, queue: String?) {
        self.uri = uri
        self.model = model
        self.driver = driver
        self.queue = queue
    }
}

public enum Drivers {
    public static func parse(_ tsv: String) -> [Driver] {
        tsv.split(separator: "\n").compactMap { line in
            if line.hasPrefix("#") { return nil }
            let f = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
            guard f.count == 6, let model = Int(f[4]) else { return nil }
            return Driver(match: f[0], name: f[1], manufacturer: f[2], filter: f[3],
                          model: model, tested: f[5] == "yes")
        }
    }

    public static func find(_ usbModel: String, in drivers: [Driver]) -> Driver? {
        drivers.first { usbModel.range(of: $0.match, options: .caseInsensitive) != nil }
    }

    public static func ppd(template: String, driver: Driver, filterPath: String) -> String {
        template
            .replacingOccurrences(of: "{{SHORT}}", with: String(driver.name.prefix(31)))  // PPD spec limit
            .replacingOccurrences(of: "{{NAME}}", with: driver.name)
            .replacingOccurrences(of: "{{MFG}}", with: driver.manufacturer)
            .replacingOccurrences(of: "{{MODEL}}", with: String(driver.model))
            .replacingOccurrences(of: "{{FILTER}}", with: filterPath)
    }
}

public enum CUPS {
    /// `lpinfo -v` lines look like `direct usb://Hewlett-Packard/HP%20LaserJet%20...?serial=...`.
    public static func usbDevices(lpinfo: String) -> [(uri: String, model: String)] {
        lpinfo.split(separator: "\n").compactMap { line in
            guard let r = line.range(of: "usb://") else { return nil }
            let uri = String(line[r.lowerBound...])
            guard let url = URL(string: uri) else { return nil }
            return (uri, String(url.path.dropFirst()))
        }
    }

    /// `LANG=C lpstat -v` lines look like `device for NAME: URI`. Returns URI → queue name.
    public static func queues(lpstat: String) -> [String: String] {
        var out: [String: String] = [:]
        for line in lpstat.split(separator: "\n") where line.hasPrefix("device for ") {
            guard let colon = line.range(of: ": ") else { continue }
            let name = line[line.index(line.startIndex, offsetBy: 11)..<colon.lowerBound]
            out[String(line[colon.upperBound...])] = String(name)
        }
        return out
    }

    public static func queueName(for model: String) -> String {
        "JPA_" + String(model.map { $0.isLetter || $0.isNumber ? $0 : "_" })
    }
}

/// Single-quotes a string for /bin/sh.
public func shellQuote(_ s: String) -> String {
    "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
}
