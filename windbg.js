"use strict";

var defaultL = false
var verbose = false

let LJ_TNIL		= 0
let LJ_TFALSE	= 1
let LJ_TTRUE	= 2
let LJ_TLIGHTUD	= 3
let LJ_TSTR		= 4
let LJ_TUPVAL	= 5
let LJ_TTHREAD	= 6
let LJ_TPROTO	= 7
let LJ_TFUNC	= 8
let LJ_TTRACE	= 9
let LJ_TCDATA	= 10
let LJ_TTAB		= 11
let LJ_TUDATA	= 12

function print(s, ...args) {
    host.diagnostics.debugLog(s, ...args, "\n")
}

function debugPrint(s, ...args) {
    if(verbose) {
        host.diagnostics.debugLog(s, ...args, "\n")
    }
}

function createLuaPointer(addr, typeName)
{
    if(typeof(typeName) != "string"){
        throw Error("Expected type name")
    }
    try{
        return host.createPointerObject(addr, "lua51.dll", typeName)
    } catch(ex) {
        let stack = new Error().stack
        print(`createLuaPointer failed error="${ex} type= ${typeName} argtype=${typeof(addr)}\n ${stack}"`)
    }
}

function printprops(o)
{
    print(Object.keys(o))
    for (var prop of Object.getOwnPropertyNames(o))
    {
        //print(prop)
    }
}

function initializeScript()
{
    print("initializeScript");
    try {
    }catch (ex ) {
        print("exceptions")
        print(ex)
    }
    return [new host.apiVersionSupport(1, 3)];
}

var protoSize = 0
var gcNextOffset = 0

function init()
{
    if(protoSize != 0) {
        return;
    }
    protoSize = host.evaluateExpression('sizeof(GCproto)');
}

function invokeScript()
{
   // host.evaluateExpression
   // ".nvload lua.natvis"
   // host.currentThread.Stack.Frames.filter.
    //
    // Insert your script content here.  This method will be called whenever the script is
    // invoked from a client.
    //
    // See the following for more details:
    //
    //     https://aka.ms/JsDbgExt
    //
}

function* findLuaFrameInStack(stack, start)
{
    //print("findLuaFrameInStack")
    for (var i = 0; i < stack.Frames.Count(); i++)
    {
        var frame = stack.Frames[i];
        //print("Frame: ", i)
        // Catch any exceptions that might occur due to inability to find PDB
        try
        {
            if (frame.toString().includes("lua51"))
            {
                print(frame.toString())
                yield frame;
            }
        }
        catch(ex)
        {
            print(ex)
        }
    }
    return {}
}

function FirstOrNull(iterator)
{
    var step = iterator.next()
    return step.done ? null : step.value
}

function* findLuaThreads()
{
    for (var thread of host.currentProcess.Threads)
    {
       // print(thread.Stack.toString())
        var frame = FirstOrNull(findLuaFrameInStack(thread.Stack))
        if (frame)
        {
            print("Found:", frame.toString())
            yield thread;
        }
    }
}

function isValidLuaState(L)
{
    try {
        //print(L.tostring())
        if(L.glref.ptr32 == 0) {
            return false;
        }
        var GG_L = L["GG State"].L;
        return L.glref.ptr32 == GG_L.glref.ptr32
    } catch(ex) {
        print("isValidLuaState: exception= ",ex)
        return false;
    }
}

function* findLInThread(thread)
{
    thread = thread || host.currentThread

    var stack = thread.Stack;
    for (let frame of stack.Frames)
    {
        // Catch any exceptions that might occur due to inability to find PDB
        try
        {
            var locals = frame.Parameters;
            var local = locals["L"];
            if (local !== undefined)
            {
                print("found L")
                yield local;
            }
        }
        catch(ex)
        {
           // print(ex,"\n")
        }
    }
}

function findLuaState()
{
    for(let thread of findLuaThreads()) {
    for (var L of findLInThread(thread))
    {
        print("loop")
        if(isValidLuaState(L)) {
            return L
        }
    }
    }
    //findLuaThreads().Where()
}

function snapshotPC(trace, snapno)
{
    if(typeof(trace) == "number") {
        trace =  createLuaPointer(trace, "GCtrace*");
    }

    if (snapno > trace.nsnap) {

        Error(`Snapshot index ${snapno} is out of range of ${trace.nsnap} `);
    }

    var snap = trace.snap[snapno];
    return trace.snapmap[snap.mapofs + snap.nent]
}

var protoList
var protoCount = 0

function binarySearch(ar, el, compare_fn) {
    var m = 0;
    var n = ar.length - 1;
    while (m <= n) {
        var k = (n + m) >> 1;
        var cmp = compare_fn(el, ar[k]);
        if (cmp > 0) {
            m = k + 1;
        } else if(cmp < 0) {
            n = k - 1;
        } else {
            return k;
        }
    }
    return -m;
}

function findClosestProto(bcAddress)
{
    init()
    var index = binarySearch(protoList, bcAddress, (addr, ptAddress) => {
        var start = ptAddress + protoSize
        var diff = addr - start
        return diff
    })

    if (index >= 0) {
        return index;
    }

    index = (-index) - 2
    for (var i = index; i < index+10; i++) {
        var pt =  createLuaPointer(protoList[i], "GCproto*");
        var start = protoList[i] + protoSize
        var diff = bcAddress - start
        //print(i, ": ", protoList[i], " diff= ", diff)
        if(diff >= 0 && diff <= (pt.sizebc *4)) {
            return i
        }
    }

}

function toFuncProto(pt)
{
    if(typeof(pt) == "number") {
        return createLuaPointer(pt, "GCproto*")
    }

    if (pt.firstline !== undefined) {
        return pt
    }

    if(pt.l !== undefined && pt.c !== undefined) {
        return createLuaPointer(pt.l.pc.ptr32 - protoSize, "GCproto*")
    }
    
    throw Error("Bad func proto value" + pt.toString())
}

// Returns the byte code index of bytecode address for the proto passed in
function proto_bcpos(pt, bcAddress)
{
    pt = toFuncProto(pt)
    let start = pt.address + protoSize
    return (bcAddress - start) / 4
}

function proto_getLineNumber(pt, pc)
{
  var lineinfo = pt.lineinfo.ptr32;
  if (pc <= pt.sizebc && lineinfo != 0) {
    let first = pt.firstline;
    if (pc == pt.sizebc) return first + pt.numline;
    if (pc-- == 0) return first;
    if (pt.numline < 256) {
      return first + host.createPointerObject(lineinfo, "lua51.dll", "unsigned char*")[pc];
    } else if (pt.numline < 65536) {
      return first + host.createPointerObject(lineinfo, "lua51.dll", "unsigned short*")[pc];
    } else {
      return first + host.createPointerObject(lineinfo, "lua51.dll", "unsigned int*")[pc];
    }
  }
  return 0;
}

function guessPTfromBC(addr)
{
    init()
    let BC_FUNCF = 90
    let BC_FUNCV = 93
   // print(addr)
    for(var i = 0; i < 1000; i++){
        var bc = host.memory.readMemoryValues(addr - (i*4), 1, 4)
        let op = bc & 0xff
        debugPrint(`${i}: op = ${op}`)
        if (op >= BC_FUNCF && op <= BC_FUNCV) {
            addr = addr - ((i*4) + protoSize)
            var pt = createLuaPointer(addr, "GCproto*");
            return pt
        }
    }
}

function findPTFromBC(addr)
{
    var index = findClosestProto(addr)

    if (index == -1) {
        Error("Failed to find PT that contains the pointer");
    }

    return createLuaPointer(protoList[index], "GCproto*");
}

function dumpTraceSnaps(trace) {
    var trace = createLuaPointer(trace, "GCtrace*");

    for (var i = 0; i < trace.nsnap; i++) {
        var pt
        var pc = snapshotPC(trace, i)
        print(`Snapshot ${i} pc= ${pc}`)
        try{
            if(true) {
                pt = guessPTfromBC(pc)
                //return pt
            } else {
                var ptIndex = findClosestProto(pc)
                pt = createLuaPointer(protoList[ptIndex], "GCproto*");
            }
            var bcindex = proto_bcpos(pt, pc)
            var line = proto_getLineNumber(pt, bcindex)
            print(`  proto: ${pt.chunkname}:${pt.firstline}, "bcindex: ${bcindex} line: ${line}`)
        } catch(ex) {
            print(ex)
        }
    }
}

let FRAME_TYPE = 3
let FRAME_P	= 4
let FRAME_TYPEP = FRAME_TYPE|FRAME_P

let FRAME_LUA    = 0
let FRAME_C      = 1
let FRAME_CONT   = 2
let FRAME_VARG   = 3
let FRAME_LUAP   = 4
let FRAME_CP     = 5
let FRAME_PCALL  = 6
let FRAME_PCALLH = 7

let FrameTypes = [
 "FRAME_LUA",
 "FRAME_C",
 "FRAME_CONT",
 "FRAME_VARG",
 "FRAME_LUAP",
 "FRAME_CP",
 "FRAME_PCALL",
 "FRAME_PCALLH",
]


function frame_gc(frame) {
    let fr = createLuaPointer(frame, "TValue*").fr
    return createLuaPointer(fr.func.gcptr32, "GCobj*")
}

function frame_ftsz(tv) {
    return createLuaPointer(tv, "TValue*").fr.tp.ftsz
}

function frame_type(f) {
    return frame_ftsz(f) & FRAME_TYPE
}

function frame_typep(f) {
    return frame_ftsz(f) & FRAME_TYPEP
}

function frame_islua(f) {
    return frame_type(f) == FRAME_LUA
}

function frame_iscont(f) {
    return frame_typep(f) == FRAME_CONT
}

function frame_pc(tv) {
    return createLuaPointer(tv, "TValue*").fr.tp.pcr.ptr32
}
function frame_contpc(f) {
    return frame_pc(f) - 4
}

function bc_a(i) {
    return ((i >> 8) & 0xff)
}

function frame_prevl(f) 
{
    let bc = host.memory.readMemoryValues(frame_pc(f) - 4, 1, 4)
    let op = bc & 0xff
    debugPrint(`frame_prevl: op= ${op} `)
    return f - (1 + bc_a(bc)) * 8
}

function frame_isvarg(f) {
    return frame_typep(f) == FRAME_VARG
}

function frame_sized(f) {
    return frame_ftsz(f) & ~FRAME_TYPEP
}

function frame_prevd(f) {
    return f - frame_sized(f)
}

function frame_previous(f) {
    if(frame_islua(f)) {
        return frame_prevl(f)
    } else {
        return frame_prevd(f)
    }
}

function func_proto(f)
{
    init()
    let pc = createLuaPointer(f, "GCfunc*").l.pc.ptr32
    return createLuaPointer(pc - protoSize, "GCproto*")
}

function isluafunc(fn) {
    return createLuaPointer(fn, "GCfunc*").c.ffid == FF_LUA
}

function isffunc(fn) {
    return createLuaPointer(fn, "GCfunc*").c.ffid > FF_C
}

function printLuaCFrame(frame)
{
    let frgco = frame_gc(frame)

    if(frgco.fn.l.ffid == 0) {
        print("C Frame contains a Lua function ffid=0")
        return
    }

    print(`  C function: ffid= ${frgco.fn.l.ffid}, func= ${frgco.fn.c.func.address}`)
    
    let Control = host.namespace.Debugger.Utility.Control;

    var output = Control.ExecuteCommand(`ln ${frgco.fn.c.func.address}`)
    if(output.length > 4 && output[4] == "Exact matches:"){
        print(output[5])
    } else {
        for (var line of output)
        {
                print(line)
        }
    }
}

function printLuaFrame(frame, pc)
{
    let frgco = frame_gc(frame)

    if(frgco.gch.gct != LJ_TFUNC) {
        print(`  Non function ${frgco.gch.gct }`)
        return
    } else if(frgco.fn.l.ffid != 0) {
        printLuaCFrame(frame)
        return
    }

    let pt = func_proto(frgco.address)
    if(pt == undefined) {
        print("Failed to read frame ", frgco.address)
    }
    if(pc && pc != 0) {
        var bcindex = proto_bcpos(pt, pc)
        var line = proto_getLineNumber(pt, bcindex)
        print(`  ${pt.chunkname}(${pt.firstline}):${line},`)
    } else {
        print(`  ${pt.chunkname}:${pt.firstline},`)
    }
    return frgco
}

function printLuaStack(L, frameIndex)
{
    var baseStart

    if(frameIndex) {
        print("Using frame index of", frameIndex)
        baseStart = L.stack.ptr32 + (frameIndex * 8) + 8
    } else {
        baseStart = L.base.address
    }
    
    let fullStackSize = (baseStart - L.stack.ptr32) /8
    var frame = baseStart - 8
    print("Fullstack: size=", fullStackSize)

    var limit = 100
    var prevPC = 0
    for(var i = 0; i < limit ;i++) {
        var frameType = frame_typep(frame)
        print(`Frame(${i}, ${frame}): Type= ${FrameTypes[frameType]} offset= ${(frame - L.stack.ptr32)/8}`)

        if(frame_islua(frame)) {
            printLuaFrame(frame, prevPC)
        } else if(frameType == FRAME_C ||frameType == FRAME_CP) {
            printLuaCFrame(frame)
        }
        prevPC = frame_pc(frame)
        if(frame <= L.stack.ptr32){
            // Reached the botton of the stack
            break;
        }
        frame = frame_previous(frame)
    }

    return frame_gc(frame)
}

function collectProtos(L)
{
    var gc = L["Global State"].gc
    print(gc)
    if (gc === undefined)
    {
        return
    }

    var limit = 1
    protoCount = 0
    protoList = new Uint32Array(5000)
    for(var o of gc["GC Objects1"])
    {
        //print(o.gch.gct, " ", o.gch.nextgc.address)
        try{
            if(o.gch.gct == 7) {
                host.diagnostics.debugLog(o.targetLocation, ",");
                if(protoCount % 8 == 0){
                    host.diagnostics.debugLog("\n")
                }
                protoList[protoCount++] = o.targetLocation;
            }
        } catch(ex) {
            print(ex,"\n")
        }
        limit++;

        if (limit > 200000) {
            break;
        }
    }

    print(`Found ${protoCount} protos out of ${limit} objects scanned`)

    protoList = protoList.subarray(0, protoCount).sort()
}
