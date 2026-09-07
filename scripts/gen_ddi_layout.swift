#!/usr/bin/swift
import Foundation

func alignUp(_ val: Int, align: Int) -> Int {
    return (val + align - 1) & ~(align - 1)
}

struct CType {
    var name: String
    var size: Int
    var align: Int
}

let tUINT = CType(name: "UINT", size: 4, align: 4)
let tINT = CType(name: "INT", size: 4, align: 4)
let tBOOL = CType(name: "BOOL", size: 4, align: 4)
let tFLOAT = CType(name: "FLOAT", size: 4, align: 4)
let tSIZE_T = CType(name: "SIZE_T", size: 8, align: 8)
let tHRESULT = CType(name: "HRESULT", size: 4, align: 4)
let tPTR = CType(name: "void*", size: 8, align: 8)

func tHandle(_ name: String) -> CType {
    return CType(name: name, size: 8, align: 8)
}

func tPointer(_ name: String) -> CType {
    return CType(name: name + "*", size: 8, align: 8)
}

@resultBuilder
struct MemberBuilder {
    static func buildBlock(_ components: LayoutElement...) -> [LayoutElement] {
        return components
    }
}

protocol LayoutElement {
    var localOffset: Int { get set }
    var size: Int { get }
    var align: Int { get }
    
    func computeLayout(currentOffset: Int) -> Int
    func generateDecl(indent: String) -> [String]
    func generateAssertions(structName: String, baseOffset: Int) -> [String]
}

class Field: LayoutElement {
    let name: String
    let type: CType
    var localOffset: Int = 0
    
    init(_ name: String, type: CType) {
        self.name = name
        self.type = type
    }
    
    convenience init(_ name: String, type: StructDef) {
        self.init(name, type: type.asType)
    }
    
    var size: Int { type.size }
    var align: Int { type.align }
    
    func computeLayout(currentOffset: Int) -> Int {
        self.localOffset = alignUp(currentOffset, align: self.align)
        return self.localOffset + self.size
    }
    
    func generateDecl(indent: String) -> [String] {
        return ["\(indent)\(type.name) \(name);"]
    }
    
    func generateAssertions(structName: String, baseOffset: Int) -> [String] {
        let absoluteOffset = baseOffset + self.localOffset
        var lines = ["WINE_DDI_ASSERT_FIELD(\(structName), \(name), \(absoluteOffset));"]
        /* A 4-byte field in a structure of pointers is where a padding
         * mistake shows up, so record its size as well as its offset. */
        if self.size == 4 {
            lines.append(
                "WINE_DDI_ASSERT_FIELD_SIZE(\(structName), \(name), \(self.size));")
        }
        return lines
    }
}

class Union: LayoutElement {
    let name: String?
    let members: [LayoutElement]
    var localOffset: Int = 0
    
    init(name: String? = nil, @MemberBuilder _ builder: () -> [LayoutElement]) {
        self.name = name
        self.members = builder()
    }

    /* An arm set built elsewhere, so one list can be shared between two
     * models of the same union. */
    init(name: String? = nil, members: [LayoutElement]) {
        self.name = name
        self.members = members
    }
    
    var size: Int {
        return members.map { $0.size }.max() ?? 0
    }
    
    var align: Int {
        return members.map { $0.align }.max() ?? 1
    }
    
    func computeLayout(currentOffset: Int) -> Int {
        self.localOffset = alignUp(currentOffset, align: self.align)
        for member in members {
            _ = member.computeLayout(currentOffset: self.localOffset)
        }
        return self.localOffset + self.size
    }
    
    func generateDecl(indent: String) -> [String] {
        var lines = ["\(indent)union", "\(indent){"]
        for member in members {
            lines.append(contentsOf: member.generateDecl(indent: indent + "    "))
        }
        if let name = name {
            lines.append("\(indent)} \(name);")
        } else {
            lines.append("\(indent)};")
        }
        return lines
    }
    
    func generateAssertions(structName: String, baseOffset: Int) -> [String] {
        var lines: [String] = []
        for member in members {
            lines.append(contentsOf: member.generateAssertions(structName: structName, baseOffset: baseOffset))
        }
        return lines
    }
}

class StructDef: LayoutElement {
    let name: String
    let members: [LayoutElement]
    var localOffset: Int = 0
    
    private var _size: Int = 0
    private var _align: Int = 1
    
    init(_ name: String, @MemberBuilder _ builder: () -> [LayoutElement]) {
        self.name = name
        self.members = builder()
        _ = self.computeLayout(currentOffset: 0)
    }
    
    var size: Int { _size }
    var align: Int { _align }
    
    func computeLayout(currentOffset: Int) -> Int {
        self.localOffset = alignUp(currentOffset, align: 1)
        self._align = members.map { $0.align }.max() ?? 1
        
        var offset = self.localOffset
        for member in members {
            offset = member.computeLayout(currentOffset: offset)
        }
        
        self._size = alignUp(offset - self.localOffset, align: self._align)
        return self.localOffset + self._size
    }
    
    func generateDecl(indent: String = "") -> [String] {
        var lines = ["typedef struct \(name)", "{"]
        for member in members {
            lines.append(contentsOf: member.generateDecl(indent: "    "))
        }
        lines.append("} \(name);")
        return lines
    }
    
    func generateAssertions(structName: String = "", baseOffset: Int = 0) -> [String] {
        let actualName = structName.isEmpty ? self.name : structName
        var lines = [
            "WINE_DDI_ASSERT_STANDARD_LAYOUT(\(actualName));",
            "WINE_DDI_ASSERT_SIZE(\(actualName), \(self.size));",
            "WINE_DDI_ASSERT_ALIGN(\(actualName), \(self.align));"
        ]
        for member in members {
            lines.append(contentsOf: member.generateAssertions(structName: actualName, baseOffset: baseOffset))
        }
        return lines
    }
    
    var asType: CType {
        return CType(name: self.name, size: self.size, align: self.align)
    }
}


/* --- Definitions ---
 *
 * The device-creation group, modelled twice.
 *
 * "Published" carries every union arm the specification's syntax blocks print.
 * "Declared" carries only the arms the pinned D3D11On12 source reads, which is
 * what the header declares under its rule 4.  Printing both is the point: the
 * arms alias, so the two models must agree on every offset and on both sizes,
 * and that is what makes dropping the unused arms a declaration choice rather
 * than a layout change.
 *
 * Sources, both retrieved 2026-09-07:
 *   learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_createdevice
 *   learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_base_args
 */

let publishedDXGIBaseFunctionArms: [LayoutElement] = [
    Field("pDXGIDDIBaseFunctions6_1", type: tPointer("DXGI1_6_1_DDI_BASE_FUNCTIONS")),
    Field("pDXGIDDIBaseFunctions6", type: tPointer("DXGI1_5_DDI_BASE_FUNCTIONS")),
    Field("pDXGIDDIBaseFunctions5", type: tPointer("DXGI1_4_DDI_BASE_FUNCTIONS")),
    Field("pDXGIDDIBaseFunctions4", type: tPointer("DXGI1_3_DDI_BASE_FUNCTIONS")),
    Field("pDXGIDDIBaseFunctions3", type: tPointer("DXGI1_2_DDI_BASE_FUNCTIONS")),
    Field("pDXGIDDIBaseFunctions2", type: tPointer("DXGI1_1_DDI_BASE_FUNCTIONS")),
    Field("pDXGIDDIBaseFunctions", type: tPointer("DXGI_DDI_BASE_FUNCTIONS")),
]

/* The driver reads DXGIBaseDDI.pDXGIDDIBaseFunctions6_1 unconditionally in
 * DeviceBase's constructor, so that is the arm the header declares. */
let declaredDXGIBaseFunctionArms: [LayoutElement] = [
    publishedDXGIBaseFunctionArms[0],
]

func dxgiDDIBaseArgs(arms: [LayoutElement]) -> StructDef {
    return StructDef("DXGI_DDI_BASE_ARGS") {
        Field("pDXGIBaseCallbacks", type: tPointer("DXGI_DDI_BASE_CALLBACKS"))
        Union(members: arms)
    }
}

let publishedDeviceFuncArms: [LayoutElement] = [
    Field("pDeviceFuncs", type: tPointer("D3D10DDI_DEVICEFUNCS")),
    Field("p10_1DeviceFuncs", type: tPointer("D3D10_1DDI_DEVICEFUNCS")),
    Field("p11DeviceFuncs", type: tPointer("D3D11DDI_DEVICEFUNCS")),
    Field("p11_1DeviceFuncs", type: tPointer("D3D11_1DDI_DEVICEFUNCS")),
    Field("pWDDM1_3DeviceFuncs", type: tPointer("D3DWDDM1_3DDI_DEVICEFUNCS")),
    Field("pWDDM2_0DeviceFuncs", type: tPointer("D3DWDDM2_0DDI_DEVICEFUNCS")),
    Field("pWDDM2_1DeviceFuncs", type: tPointer("D3DWDDM2_1DDI_DEVICEFUNCS")),
    Field("pWDDM2_2DeviceFuncs", type: tPointer("D3DWDDM2_2DDI_DEVICEFUNCS")),
    Field("pWDDM2_6DeviceFuncs", type: tPointer("D3DWDDM2_6DDI_DEVICEFUNCS")),
]

let publishedCoreLayerArms: [LayoutElement] = [
    Field("pUMCallbacks", type: tPointer("const D3D10DDI_CORELAYER_DEVICECALLBACKS")),
    Field("p11UMCallbacks", type: tPointer("const D3D11DDI_CORELAYER_DEVICECALLBACKS")),
    Field("pWDDM2_0UMCallbacks", type: tPointer("const D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS")),
    Field("pWDDM2_2UMCallbacks", type: tPointer("const D3DWDDM2_2DDI_CORELAYER_DEVICECALLBACKS")),
    Field("pWDDM2_6UMCallbacks", type: tPointer("const D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS")),
]

/* GetDeviceFuncsFromCreateArgs returns pWDDM2_6DeviceFuncs and DeviceBase
 * initializes m_pCallbacks from pWDDM2_6UMCallbacks, for both advertised
 * versions.  Those are the two arms the header declares. */
let declaredDeviceFuncArms: [LayoutElement] = [publishedDeviceFuncArms[8]]
let declaredCoreLayerArms: [LayoutElement] = [publishedCoreLayerArms[4]]

func d3d10ddiArgCreateDevice(deviceFuncArms: [LayoutElement],
                             coreLayerArms: [LayoutElement],
                             dxgiArms: [LayoutElement]) -> StructDef {
    return StructDef("D3D10DDIARG_CREATEDEVICE") {
        Field("hRTDevice", type: tHandle("D3D10DDI_HRTDEVICE"))
        Field("Interface", type: tUINT)
        Field("Version", type: tUINT)
        Field("pKTCallbacks", type: tPointer("const D3DDDI_DEVICECALLBACKS"))
        Union(members: deviceFuncArms)
        Field("hDrvDevice", type: tHandle("D3D10DDI_HDEVICE"))
        Field("DXGIBaseDDI", type: dxgiDDIBaseArgs(arms: dxgiArms))
        Field("hRTCoreLayer", type: tHandle("D3D10DDI_HRTCORELAYER"))
        Union(members: coreLayerArms)
        Field("Flags", type: tUINT)
        Field("ppfnRetrieveSubObject", type: tPointer("PFND3D10DDI_RETRIEVESUBOBJECT"))
    }
}

let publishedBaseArgs = dxgiDDIBaseArgs(arms: publishedDXGIBaseFunctionArms)
let declaredBaseArgs = dxgiDDIBaseArgs(arms: declaredDXGIBaseFunctionArms)
let publishedCreateDevice = d3d10ddiArgCreateDevice(
        deviceFuncArms: publishedDeviceFuncArms,
        coreLayerArms: publishedCoreLayerArms,
        dxgiArms: publishedDXGIBaseFunctionArms)
let declaredCreateDevice = d3d10ddiArgCreateDevice(
        deviceFuncArms: declaredDeviceFuncArms,
        coreLayerArms: declaredCoreLayerArms,
        dxgiArms: declaredDXGIBaseFunctionArms)

print("/* --- GENERATED DDI LAYOUT --- */\n")
print(declaredBaseArgs.generateDecl().joined(separator: "\n"))
print("\n" + declaredBaseArgs.generateAssertions().joined(separator: "\n"))
print("\n" + declaredCreateDevice.generateDecl().joined(separator: "\n"))
print("\n" + declaredCreateDevice.generateAssertions().joined(separator: "\n"))
print("\n/* ---------------------------- */")

/* Dropping the unused arms must not move anything. */
var mismatches: [String] = []
for (published, declared) in [(publishedBaseArgs, declaredBaseArgs),
                              (publishedCreateDevice, declaredCreateDevice)] {
    if published.size != declared.size || published.align != declared.align {
        mismatches.append("\(published.name): the published model is "
                + "\(published.size)/\(published.align) bytes and the declared "
                + "model is \(declared.size)/\(declared.align)")
    }
    let publishedOffsets = Dictionary(uniqueKeysWithValues:
            published.generateAssertions().compactMap { line -> (String, String)? in
                guard line.hasPrefix("WINE_DDI_ASSERT_FIELD(") else { return nil }
                let parts = line.split(separator: ",")
                return (String(parts[1]), String(parts[2]))
            })
    for line in declared.generateAssertions()
            where line.hasPrefix("WINE_DDI_ASSERT_FIELD(") {
        let parts = line.split(separator: ",")
        let field = String(parts[1])
        if let offset = publishedOffsets[field], offset != String(parts[2]) {
            mismatches.append("\(published.name).\(field): published at"
                    + "\(offset), declared at\(String(parts[2]))")
        }
    }
}
if mismatches.isEmpty {
    print("\n/* the published and declared arm sets agree on every offset */")
} else {
    for mismatch in mismatches {
        FileHandle.standardError.write(Data((mismatch + "\n").utf8))
    }
    exit(1)
}
