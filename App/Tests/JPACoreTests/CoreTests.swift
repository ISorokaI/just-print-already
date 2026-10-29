import Testing
@testable import JPACore

let tsv = """
# match\tname\tmanufacturer\tfilter\tmodel\ttested
M113\tHP LaserJet Professional M1130 MFP series\tHP\trastertozjs\t4\tyes
P1102\tHP LaserJet Professional P1102\tHP\trastertozjs\t2\tno
"""

@Test func matchesUSBModel() throws {
    let drivers = Drivers.parse(tsv)
    #expect(drivers.count == 2)
    let lpinfo = """
    network ipp
    direct usb://Hewlett-Packard/HP%20LaserJet%20Professional%20M1132%20MFP?serial=000000000QHA3DAKPR1a
    """
    let dev = try #require(CUPS.usbDevices(lpinfo: lpinfo).first)
    #expect(dev.model == "HP LaserJet Professional M1132 MFP")
    #expect(Drivers.find(dev.model, in: drivers)?.model == 4)
    #expect(Drivers.find("HP LaserJet Professional P1102w", in: drivers)?.model == 2)
    #expect(Drivers.find("Pantum P2500W series", in: drivers) == nil)
}

@Test func parsesQueues() {
    let q = CUPS.queues(lpstat: "device for JPA_HP: usb://HP/X?serial=1\ndevice for Canon: dnssd://c")
    #expect(q["usb://HP/X?serial=1"] == "JPA_HP")
    #expect(CUPS.queueName(for: "HP LaserJet M1132") == "JPA_HP_LaserJet_M1132")
}

@Test func fillsPPD() {
    let d = Drivers.parse(tsv)[0]
    let ppd = Drivers.ppd(template: "*cupsModelNumber: {{MODEL}}\n*cupsFilter: \"x 0 {{FILTER}}\"",
                          driver: d, filterPath: "/Library/Printers/JustPrintAlready/rastertozjs")
    #expect(ppd == "*cupsModelNumber: 4\n*cupsFilter: \"x 0 /Library/Printers/JustPrintAlready/rastertozjs\"")
    #expect(shellQuote("a'b") == "'a'\\''b'")
}
