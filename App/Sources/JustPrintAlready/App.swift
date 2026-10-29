import JPACore
import SwiftUI

@main
struct JustPrintAlreadyApp: App {
    var body: some Scene {
        WindowGroup("Just Print Already") { ContentView() }
            .windowResizability(.contentSize)
    }
}

let installDir = "/Library/Printers/JustPrintAlready"

func resource(_ name: String) -> URL {
    Bundle.main.resourceURL!.appendingPathComponent(name)
}

/// Runs a tool with the C locale so CUPS output is parseable.
func run(_ path: String, _ args: [String]) throws -> String {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: path)
    p.arguments = args
    p.environment = ["LANG": "C", "LC_ALL": "C"]
    let out = Pipe(), err = Pipe()
    p.standardOutput = out
    p.standardError = err
    try p.run()
    let data = out.fileHandleForReading.readDataToEndOfFile()
    let errData = err.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    guard p.terminationStatus == 0 else {
        throw NSError(domain: "JPA", code: Int(p.terminationStatus), userInfo: [
            NSLocalizedDescriptionKey: String(decoding: errData, as: UTF8.self)
        ])
    }
    return String(decoding: data, as: UTF8.self)
}

func scan() throws -> [Printer] {
    let drivers = Drivers.parse(try String(contentsOf: resource("models.tsv"), encoding: .utf8))
    let queues = CUPS.queues(lpstat: (try? run("/usr/bin/lpstat", ["-v"])) ?? "")
    return CUPS.usbDevices(lpinfo: try run("/usr/sbin/lpinfo", ["-v"])).map {
        Printer(uri: $0.uri, model: $0.model, driver: Drivers.find($0.model, in: drivers),
                queue: queues[$0.uri].flatMap { $0.hasPrefix("JPA_") ? $0 : nil })
    }
}

/// Copies the filter and creates the queue, behind one admin password prompt.
func setUp(_ printer: Printer, driver: Driver) throws -> String {
    let template = try String(contentsOf: resource("zjs/template.ppd"), encoding: .utf8)
    let ppd = FileManager.default.temporaryDirectory.appendingPathComponent("jpa-\(UUID().uuidString).ppd")
    try Drivers.ppd(template: template, driver: driver, filterPath: "\(installDir)/\(driver.filter)")
        .write(to: ppd, atomically: true, encoding: .utf8)
    defer { try? FileManager.default.removeItem(at: ppd) }

    let queue = CUPS.queueName(for: printer.model)
    let cmd = ["/bin/sh", resource("install-queue.sh").path, resource("filters/\(driver.filter)").path,
               ppd.path, queue, printer.uri, printer.model].map(shellQuote).joined(separator: " ")
    let script = "do shell script \"\(cmd.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"
    _ = try run("/usr/bin/osascript", ["-e", script])
    return queue
}

struct ContentView: View {
    @State private var printers: [Printer] = []
    @State private var busy = false
    @State private var message = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Принтеры на USB").font(.headline)
                Spacer()
                Button("Обновить", action: refresh).disabled(busy)
            }
            if printers.isEmpty {
                Text("Подключите принтер по USB и нажмите «Обновить».").foregroundStyle(.secondary)
            }
            ForEach(printers) { p in row(p) }
            if !message.isEmpty {
                Text(message).font(.callout).foregroundStyle(.secondary).textSelection(.enabled)
            }
        }
        .padding(20)
        .frame(width: 560, alignment: .topLeading)
        .task { refresh() }
    }

    @ViewBuilder func row(_ p: Printer) -> some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 2) {
                Text(p.model)
                Text(status(p)).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if let queue = p.queue {
                Button("Тестовая страница") { work { _ = try run("/usr/bin/lp", ["-d", queue, resource("testpage.pdf").path]); return "Страница отправлена на \(queue)." } }
            } else if let d = p.driver {
                Button("Настроить") { work { "Готово: печатайте на «\(try setUp(p, driver: d))»." } }
                    .buttonStyle(.borderedProminent)
            }
        }
        .disabled(busy)
        .padding(10)
        .background(.quaternary.opacity(0.5), in: .rect(cornerRadius: 8))
    }

    func status(_ p: Printer) -> String {
        if p.queue != nil { return "Настроен" }
        guard let d = p.driver else { return "Пока не поддерживается" }
        return "Драйвер: \(d.name)" + (d.tested ? "" : " (не проверен на живом принтере)")
    }

    func refresh() {
        work { "" }
    }

    /// Runs `job` off the main thread, then rescans the printers.
    func work(_ job: @escaping @Sendable () throws -> String) {
        busy = true
        Task.detached {
            var result: String
            do { result = try job() } catch { result = "Ошибка: \(error.localizedDescription)" }
            let fresh: [Printer]
            do { fresh = try scan() } catch { fresh = []; result = "Ошибка: \(error.localizedDescription)" }
            let text = result
            await MainActor.run {
                message = text
                printers = fresh
                busy = false
            }
        }
    }
}
