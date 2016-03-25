// Copyright (c) 2015 Quanta Research Cambridge, Inc.

// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

import ToolConfig::*;
import Vector::*;
import BuildVector::*;
import Portal::*;
import HostInterface::*;
import MemServer::*;
import MemTypes::*;
import CtrlMux::*;
import FIFO::*;
import GetPut::*;
import SpecialFIFOs::*;
import Pipe::*;
import ToolMemory::*;
import MemServerIndication::*;
import MemServerRequest::*;
import IfcNames::*;
`include "ProjectConfig.bsv"
import `PinTypeInclude::*;

interface Platform;
   interface PhysMemSlave#(32,32) slave;
   interface Vector#(NumberOfMasters,PhysMemMaster#(PhysAddrWidth, DataBusWidth)) masters;
   interface Vector#(MaxNumberOfPortals,ReadOnly#(Bool)) interrupt;
   interface `PinType pins;
endinterface

typedef TMax#(TLog#(TSub#(NumberOfTiles,1)),1) TileTagBits;
function Bit#(TSub#(MemTagSize,TileTagBits)) tagLsb(Bit#(MemTagSize) tag); return truncate(tag); endfunction
function Bit#(TileTagBits) tagMsb(Bit#(MemTagSize) tag); return truncate(tag >> valueOf(TSub#(MemTagSize,TileTagBits))); endfunction

module renameReads#(Integer tile, PhysMemReadClient#(PhysAddrWidth,DataBusWidth) reader, MemServerIndication err)(PhysMemReadClient#(PhysAddrWidth,DataBusWidth));
   interface Get readReq;
      method ActionValue#(PhysMemRequest#(PhysAddrWidth,DataBusWidth)) get;
         let req <- reader.readReq.get;
         Bit#(TSub#(MemTagSize,TileTagBits)) lsb = tagLsb(req.tag);
         Bit#(TileTagBits) msb = tagMsb(req.tag);
         if(req.tag != extend(lsb) && valueOf(NumberOfTiles) > 2) begin // one mgmt tile and one user tile
            $display("renameReads tile tag out of range: 'h%h", req.tag);
            err.error(extend(pack(DmaErrorTileTagOutOfRange)), extend(req.tag), fromInteger(tile));
         end
         req.tag = {fromInteger(tile),lsb};
         return req;
      endmethod
   endinterface
   interface Put readData;
      method Action put(MemData#(DataBusWidth) v);
         reader.readData.put(MemData{data:v.data, tag:{0,tagLsb(v.tag)}, last:v.last});
      endmethod
   endinterface
endmodule

module renameWrites#(Integer tile, PhysMemWriteClient#(PhysAddrWidth,DataBusWidth) writer, MemServerIndication err)(PhysMemWriteClient#(PhysAddrWidth,DataBusWidth));
   interface Get writeReq;
      method ActionValue#(PhysMemRequest#(PhysAddrWidth,DataBusWidth)) get;
         let req <- writer.writeReq.get;
         Bit#(TSub#(MemTagSize,TileTagBits)) lsb = tagLsb(req.tag);
         Bit#(TileTagBits) msb = tagMsb(req.tag);
         if(req.tag != extend(lsb) && valueOf(NumberOfTiles) > 2) begin // one mgmt tile and one user tile
            $display("renameWrites tile tag out of range: 'h%h", req.tag);
            err.error(extend(pack(DmaErrorTileTagOutOfRange)), extend(req.tag), fromInteger(tile));
         end
         req.tag = {fromInteger(tile),lsb};
         return req;
      endmethod
   endinterface
   interface Get writeData;
      method ActionValue#(MemData#(DataBusWidth)) get;
         let rv <- writer.writeData.get;
            return MemData{data:rv.data, tag:{0,tagLsb(rv.tag)}, last:rv.last};
      endmethod
   endinterface
   interface Put writeDone;
      method Action put(Bit#(MemTagSize) v);
         writer.writeDone.put({0,tagLsb(v)});
      endmethod
   endinterface
endmodule

module mkPlatform#(ConnectalTop tiles)(Platform);
   /////////////////////////////////////////////////////////////
   // connecting up the tiles

   PhysMemSlave#(18,32) tile_slaves = tiles.slave;
   let imux <- mkInterruptMux(tiles.interrupt);
   //ReadOnly#(Bool) imux = tiles.interrupt;
   ReadOnly#(Bool) tile_interrupts = imux;
   Vector#(NumReadClients, PhysMemReadClient#(PhysAddrWidth,DataBusWidth)) tile_read_clients = tiles.readers;
   Vector#(NumWriteClients, PhysMemWriteClient#(PhysAddrWidth,DataBusWidth)) tile_write_clients = tiles.writers;
   Vector#(NumReadClients, Integer) read_client_tile_numbers = replicate(0);
   Vector#(NumWriteClients, Integer) write_client_tile_numbers = replicate(0);
 
   /////////////////////////////////////////////////////////////
   // framework internal portals

   MemServerIndicationProxy lMemServerIndicationProxy <- mkMemServerIndicationProxy(PlatformIfcNames_MemServerIndicationH2S);

   Vector#(NumReadClients, PhysMemReadClient#(PhysAddrWidth,DataBusWidth)) tile_read_clients_renamed <- zipWith3M(renameReads, (read_client_tile_numbers), (tile_read_clients), replicate(lMemServerIndicationProxy.ifc));
   Vector#(NumWriteClients, PhysMemWriteClient#(PhysAddrWidth,DataBusWidth)) tile_write_clients_renamed <- zipWith3M(renameWrites, (write_client_tile_numbers), (tile_write_clients), replicate(lMemServerIndicationProxy.ifc));
   MemServer#(PhysAddrWidth,DataBusWidth,NumberOfMasters) lMemServer <- mkMemServer(tile_read_clients_renamed, tile_write_clients_renamed, lMemServerIndicationProxy.ifc);

   MemServerRequestWrapper lMemServerRequestWrapper <- mkMemServerRequestWrapper(PlatformIfcNames_MemServerRequestS2H, lMemServer.request);

   Vector#(2,StdPortal) framework_portals;
   framework_portals[0] = lMemServerIndicationProxy.portalIfc;
   framework_portals[1] = lMemServerRequestWrapper.portalIfc;
   PhysMemSlave#(18,32) framework_ctrl_mux <- mkSlaveMux(framework_portals);
   let framework_intr <- mkInterruptMux(getInterruptVector(framework_portals));
   
   /////////////////////////////////////////////////////////////
   // expose interface to top

   PhysMemSlave#(32,32) ctrl_mux <- mkPhysMemSlaveMux(vec(framework_ctrl_mux,tile_slaves));
   Vector#(MaxNumberOfPortals, ReadOnly#(Bool)) interrupts = replicate(interface ReadOnly; method Bool _read(); return False; endmethod endinterface);
   interrupts[0] = framework_intr;
   interrupts[1] = tile_interrupts;
   interface interrupt = interrupts;
   interface slave = ctrl_mux;
   interface masters = lMemServer.masters;
   interface pins = tiles.pins;
endmodule
