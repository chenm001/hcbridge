#!/usr/bin/python
## Copyright (c) 2013-2014 Quanta Research Cambridge, Inc.

## Permission is hereby granted, free of charge, to any person
## obtaining a copy of this software and associated documentation
## files (the "Software"), to deal in the Software without
## restriction, including without limitation the rights to use, copy,
## modify, merge, publish, distribute, sublicense, and/or sell copies
## of the Software, and to permit persons to whom the Software is
## furnished to do so, subject to the following conditions:

## The above copyright notice and this permission notice shall be
## included in all copies or substantial portions of the Software.

## THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
## EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
## MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
## NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
## BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
## ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
## CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
## SOFTWARE.
import os, sys, shutil, string
import argparse
import util

def newArgparser():
    argparser = argparse.ArgumentParser("Generate Top.bsv for an project.")
    argparser.add_argument('--project-dir', help='project directory')
    argparser.add_argument('--interface', default=[], help='exported interface declaration', action='append')
    argparser.add_argument('--board', help='Board type')
    argparser.add_argument('--importfiles', default=[], help='added imports', action='append')
    argparser.add_argument('--portname', default=[], help='added portal names to enum list', action='append')
    argparser.add_argument('--wrapper', default=[], help='exported wrapper interfaces', action='append')
    argparser.add_argument('--proxy', default=[], help='exported proxy interfaces', action='append')
    argparser.add_argument('--memread', default=[], help='memory read interfaces', action='append')
    argparser.add_argument('--memwrite', default=[], help='memory read interfaces', action='append')
    argparser.add_argument('--integratedIndication', help='indication pipes instantiated in user module', action='store_true')
    return argparser

argparser = newArgparser()

topTemplate='''
import ToolConfig::*;
import Vector::*;
import Portal::*;
import CtrlMux::*;
import HostInterface::*;
import Connectable::*;
import MemTypes::*;
import MemServer::*;
import IfcNames::*;
%(generatedImport)s
`include "ProjectConfig.bsv"

`ifndef IMPORT_HOSTIF
(* synthesize *)
`endif
module mkConnectalTop
`ifdef IMPORT_HOSTIF // no synthesis boundary
      #(HostInterface host)
`else
`ifdef IMPORT_HOST_CLOCKS // enables synthesis boundary
       #(Clock derivedClockIn, Reset derivedResetIn)
`else
// otherwise no params
`endif
`endif
       (%(moduleParam)s);
   Clock defaultClock <- exposeCurrentClock();
   Reset defaultReset <- exposeCurrentReset();
`ifdef IMPORT_HOST_CLOCKS // enables synthesis boundary
   HostInterface host = (interface HostInterface;
                           interface Clock derivedClock = derivedClockIn;
                           interface Reset derivedReset = derivedResetIn;
                         endinterface);
`endif
%(pipeInstantiate)s

%(portalInstantiate)s
%(connectInstantiate)s

   Vector#(%(portalCount)s,StdPortal) portals;
%(portalList)s
   let ctrl_mux <- mkSlaveMux(portals);
   Vector#(NumWriteClients,PhysMemWriteClient#(PhysAddrWidth,DataBusWidth)) nullWriters = replicate(null_phys_mem_write_client());
   Vector#(NumReadClients,PhysMemReadClient#(PhysAddrWidth,DataBusWidth)) nullReaders = replicate(null_phys_mem_read_client());
   interface interrupt = getInterruptVector(portals);
   interface slave = ctrl_mux;
   interface readers = take(%(portalReaders)s);
   interface writers = take(%(portalWriters)s);
%(pinsInterface)s
%(exportedInterfaces)s
endmodule : mkConnectalTop
%(exportedNames)s
'''

ifcnamesTemplate='''
typedef enum {NoInterface, %(enumList)s} IfcNames deriving (Eq,Bits);
'''

topEnumTemplate='''
typedef enum {NoInterface, %(enumList)s} IfcNames;
'''

portalTemplate = '''   PortalCtrlMemSlave#(SlaveControlAddrWidth,SlaveDataBusWidth) ctrlPort_%(count)s <- mkPortalCtrlMemSlave(extend(pack(%(enumVal)s)), %(ifcName)s.intr);
   let memslave_%(count)s <- mkMemMethodMux%(slaveType)s(ctrlPort_%(count)s.memSlave,%(ifcName)s.%(itype)s);
   portals[%(count)s] = (interface MemPortal;
       interface PhysMemSlave slave = memslave_%(count)s;
       interface ReadOnly interrupt = ctrlPort_%(count)s.interrupt;
       interface WriteOnly num_portals = ctrlPort_%(count)s.num_portals;
       endinterface);'''

def addPortal(outputPrefix, enumVal, ifcName, direction):
    global portalCount
    iName = ifcName + '.portalIfc'
    if outputPrefix != '':
        iName = outputPrefix + ifcName
    portParam = {'count': portalCount, 'enumVal': enumVal, 'ifcName': iName, 'ifcNameNoc': ifcName + 'Noc', 'direction': direction}
    if direction == 'Request':
        requestList.append('%(ifcNameNoc)s' % portParam)
        portParam['itype'] = 'requests'
        portParam['slaveType'] = 'In'
        portParam['intrParam'] = ''
        portParam['messageSize'] = ''
    else:
        indicationList.append('%(ifcNameNoc)s' % portParam)
        portParam['itype'] = 'indications'
        portParam['slaveType'] = 'Out'
        portParam['intrParam'] = ', %(ifcName)s.intr' % portParam
        portParam['messageSize'] = ', %(ifcName)s.messageSize' % portParam
    p = portalTemplate
    portalList.append(p % portParam)
    portalCount = portalCount + 1

class iReq:
    def __init__(self):
        self.inst = ''
        self.args = []

pipeInstantiation = '''   %(modname)s%(tparam)s l%(modname)s%(number)s <- mk%(modname)s;'''

connectInstantiation = '''   mkConnection(l%(modname)s%(number)s.pipes, l%(userIf)s);'''

def instMod(pmap, args, modname, modext, constructor, tparam):
    if not modname:
        return
    map = pmap.copy()
    pmap['tparam'] = tparam
    pmap['modname'] = modname + modext
    pmap['modnamebase'] = modname
    tstr = 'S2H'
    if modext == 'Output':
        tstr = 'H2S'
    if modext:
        args = modname + tstr
    pmap['args'] = args % pmap
    if modext:
        options.portname.append('IfcNames_' + modname + tstr + pmap['number'])
        pmap['argsConfig'] = modname + tstr
        outputPrefix = ''
        if modext == 'Output':
            pmap['stype'] = 'Indication';
        else:
            pmap['stype'] = 'Request';
        if modext == 'Output':
            if options.integratedIndication:
                outputPrefix = 'l' + pmap['usermod'] + '.'
            else:
                pipeInstantiate.append(pipeInstantiation % pmap)
        else:
            pipeInstantiate.append(pipeInstantiation % pmap)
            connectInstantiate.append(connectInstantiation % pmap)
        addPortal(outputPrefix, 'IfcNames_' + pmap['args'] + pmap['number'], 'l%(modname)s%(number)s' % pmap, pmap['stype'])
    else:
        if not instantiateRequest.get(pmap['modname']):
            instantiateRequest[pmap['modname']] = iReq()
            pmap['hostif'] = ''
            instantiateRequest[pmap['modname']].inst = '   %(modname)s%(tparam)s l%(modname)s <- mk%(modname)s(%(hostif)s%%s);' % pmap
        instantiateRequest[pmap['modname']].args.append(pmap['args'])
    if pmap['modname'] not in instantiatedModules:
        instantiatedModules.append(pmap['modname'])
    options.importfiles.append(modname)

def flushModules(key):
        temp = instantiateRequest.get(key)
        if temp:
            portalInstantiate.append(temp.inst % ','.join(temp.args))
            del instantiateRequest[key]

def toVectorLiteral(l):
    if l:
        return 'cons(%s,%s)' % (l[0], toVectorLiteral(l[1:]))
    else:
        return 'nil'

def parseParam(pitem, proxy):
    p = pitem.split(':')
    pmap = {'tparam': '', 'xparam': '', 'uparam': ''}
    print 'pmap=', pmap
    pmap['usermod'] = p[0].replace('/','').replace('!','')
    pmap['name'] = p[1]
    ind = pmap['usermod'].find('#')
    if ind > 0:
        pmap['xparam'] = pmap['usermod'][ind:]
        pmap['usermod'] = pmap['usermod'][:ind]
    if len(p) > 2 and p[2]:
        pmap['uparam'] = p[2] + ', '
    return pmap

if __name__=='__main__':
    options = argparser.parse_args()

    if not options.project_dir:
        print "topgen: --project-dir option missing"
        sys.exit(1)
    project_dir = os.path.abspath(os.path.expanduser(options.project_dir))
    userFiles = []
    portalInstantiate = []
    pipeInstantiate = []
    connectInstantiate = []
    instantiateRequest = {}
    for item in ['PlatformIfcNames_MemServerRequestS2H', 'PlatformIfcNames_MemServerIndicationH2S']:
        options.portname.append(item)
    requestList = []
    indicationList = []
    portalList = []
    portalCount = 0
    instantiatedModules = []
    exportedNames = []
    options.importfiles.append('`PinTypeInclude')
    exportedNames.extend(['export mkConnectalTop;'])
    if options.importfiles:
        for item in options.importfiles:
             exportedNames.append('export %s::*;' % item)
    interfaceList = []

    modcount = {}
    for pitem in options.proxy:
        print 'options.proxy: %s' % options.proxy
        pmap = parseParam(pitem, True)
        ptemp = pmap['name'].split(',')
        for pmap['name'] in ptemp:
            pmap['number'] = ''
            if (ptemp.count(pmap['name']) > 1):
                if pmap['name'] in modcount:
                    pmap['number'] = str(modcount[pmap['name']])
                    modcount[pmap['name']] += 1
                else:
                    modcount[pmap['name']] = 1
                    pmap['number'] = str(0)
            instMod(pmap, '', pmap['name'], 'Output', '', '')
            argstr = pmap['uparam']
            if not options.integratedIndication:
                argstr += ('l%(name)sOutput%(number)s.ifc')
            if pmap['uparam'] and pmap['uparam'][0] == '/':
                argstr = 'l%(name)sOutput%(number)s.ifc, ' + pmap['uparam'][1:-2]
            instMod(pmap, argstr, pmap['usermod'], '', '', pmap['xparam'])
            pmap['uparam'] = ''
    modcount = {}
    for pitem in options.wrapper:
        pmap = parseParam(pitem, False)
        print 'options.wrapper: %s %s' % (pitem, pmap)
        pmap['userIf'] = pmap['name']
        pmap['name'] = pmap['usermod']
        pmap['number'] = ''
        modintf_list = pmap['userIf'].split(',')
        number = 0
        for pmap['userIf'] in modintf_list:
            if len(modintf_list) > 1:
                pmap['number'] = str(number)
            number += 1
            pmap['usermod'] = pmap['userIf'].split('.')[0]
            if pmap['usermod'] not in instantiatedModules:
                instMod(pmap, pmap['uparam'], pmap['usermod'], '', '', pmap['xparam'])
            flushModules(pmap['usermod'])
            instMod(pmap, '', pmap['name'], 'Input', '', '')
            portalInstantiate.append('')
    for key in instantiatedModules:
        flushModules(key)
    for pitem in options.interface:
        p = pitem.split(':')
        interfaceList.append('   interface %s = l%s;' % (p[0], p[1]))

    memory_flag = 'MemServer' in instantiatedModules
    topsubsts = {'enumList': ','.join(options.portname),
                 'generatedImport': '\n'.join(['import %s::*;' % p for p in options.importfiles]),
                 'generatedTypedefs': '\n'.join(['typedef %d NumberOfRequests;' % len(requestList),
                                                 'typedef %d NumberOfIndications;' % len(indicationList)]),
                 'pipeInstantiate' : '\n'.join(sorted(pipeInstantiate)),
                 'connectInstantiate' : '\n'.join(sorted(connectInstantiate)),
                 'portalInstantiate' : '\n'.join(portalInstantiate),
                 'portalList': '\n'.join(portalList),
                 'portalCount': portalCount,
                 'requestList': toVectorLiteral(requestList),
                 'indicationList': toVectorLiteral(indicationList),
                 'exportedInterfaces' : '\n'.join(interfaceList),
                 'exportedNames' : '\n'.join(exportedNames),
                 'portalReaders' : ('append(' if len(options.memread) > 0 else '(') + ', '.join(options.memread + ['nullReaders']) + ')',
                 'portalWriters' : ('append(' if len(options.memwrite) > 0 else '(') + ', '.join(options.memwrite + ['nullWriters']) + ')',
                 'portalMaster' : 'lMemServer.masters' if memory_flag else 'nil',
#TODO: add a flag to enable pins interface                 
                 'pinsInterface' : '    interface pins = l%(usermod)s.pins;\n' % pmap if False else '',
                 'moduleParam' : 'ConnectalTop'
                 }
    topFilename = project_dir + '/Top.bsv'
    print 'Writing:', topFilename
    top = util.createDirAndOpen(topFilename, 'w')
    top.write(topTemplate % topsubsts)
    top.close()
    topFilename = project_dir + '/IfcNames.bsv'
    print 'Writing:', topFilename
    top = util.createDirAndOpen(topFilename, 'w')
    top.write(ifcnamesTemplate % topsubsts)
    top.close()
    topFilename = project_dir + '/../jni/topEnum.h'
    print 'Writing:', topFilename
    top = util.createDirAndOpen(topFilename, 'w')
    top.write(topEnumTemplate % topsubsts)
    top.close()
