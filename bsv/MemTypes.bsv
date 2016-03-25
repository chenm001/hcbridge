// Copyright (c) 2013 Quanta Research Cambridge, Inc.

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
import FIFOF::*;
import Adapter::*;
import Vector::*;
import Connectable::*;
import BRAMFIFO::*;
import GetPut::*;
import ClientServer::*;

import ToolConfig::*;
import Pipe::*;
import ToolMemory::*;
import DefaultValue::*;

`include "ProjectConfig.bsv"

typedef 44 MemOffsetSize;
`ifdef MemTagSize
typedef `MemTagSize MemTagSize;
`else
typedef 6 MemTagSize;
`endif
typedef `BurstLenSize BurstLenSize;

`ifndef USE_ACP
`ifdef PCIE3
// as configured, the Xilinx gen3 PCIe core supports 5 bit tags, and
// we need to use unique tags for all transactions in flight. Since
// MemServer uses the same tag numbers for reads and writes, we use
// tag[4] to distinguish the two, leaving 4 bits for unique tags.
// TODO: There is an option for longer tags in the gen3 core.
typedef 16 MemServerTags;
`else
typedef 32 MemServerTags;
`endif
`else
typedef 8 MemServerTags;
`endif
`ifdef DataBusWidth
typedef TDiv#(`DataBusWidth,8) ByteEnableSize;
`else
typedef TDiv#(64,8) ByteEnableSize;
`endif

// memory request with physical addresses.
// these can be transmitted directly to the bus master
typedef struct {
   Bit#(addrWidth) addr;
   Bit#(BurstLenSize) burstLen;
   Bit#(MemTagSize) tag;
`ifdef BYTE_ENABLES
   Bit#(TDiv#(dataBusWidth,8)) firstbe; // maybe we only need lastbe
   Bit#(TDiv#(dataBusWidth,8)) lastbe;
`endif
   } PhysMemRequest#(numeric type addrWidth, numeric type dataBusWidth) deriving (Bits);

instance DefaultValue#(PhysMemRequest#(addrWidth,dataBusWidth));
   defaultValue = PhysMemRequest { addr: 0, burstLen: 0, tag: 0
`ifdef BYTE_ENABLES
                                  , firstbe: maxBound, lastbe: maxBound
`endif
      };
endinstance

// memory payload
typedef struct {
   Bit#(dsz) data;
   Bit#(MemTagSize) tag;
   Bool last;
   } MemData#(numeric type dsz) deriving (Bits);

typeclass ReqByteEnables#(type t, numeric type besz);
   function Bit#(besz) reqFirstByteEnable(t req);
   function Bit#(besz) reqLastByteEnable(t req);
endtypeclass
instance ReqByteEnables#(PhysMemRequest#(addrWidth,dataBusWidth),TDiv#(dataBusWidth,8));
`ifdef BYTE_ENABLES
   function Bit#(TDiv#(dataBusWidth,8)) reqFirstByteEnable(PhysMemRequest#(addrWidth,dataBusWidth) req); return req.firstbe; endfunction
   function Bit#(TDiv#(dataBusWidth,8)) reqLastByteEnable(PhysMemRequest#(addrWidth,dataBusWidth) req); return req.lastbe; endfunction
`else
   function Bit#(TDiv#(dataBusWidth,8)) reqFirstByteEnable(PhysMemRequest#(addrWidth,dataBusWidth) req); return maxBound; endfunction
   function Bit#(TDiv#(dataBusWidth,8)) reqLastByteEnable(PhysMemRequest#(addrWidth,dataBusWidth) req); return maxBound; endfunction
`endif
endinstance

///////////////////////////////////////////////////////////////////////////////////
//

typedef struct {
   Bit#(dsz) data;
   Bit#(MemTagSize) tag;
   Bool first;
   Bool last;
   } MemDataF#(numeric type dsz) deriving (Bits);

//
///////////////////////////////////////////////////////////////////////////////////


//
///////////////////////////////////////////////////////////////////////////////////
//

interface PhysMemSlave#(numeric type addrWidth, numeric type dataWidth);
   interface PhysMemReadServer#(addrWidth, dataWidth) read_server;
   interface PhysMemWriteServer#(addrWidth, dataWidth) write_server;
endinterface

interface PhysMemMaster#(numeric type addrWidth, numeric type dataWidth);
   interface PhysMemReadClient#(addrWidth, dataWidth) read_client;
   interface PhysMemWriteClient#(addrWidth, dataWidth) write_client;
endinterface

interface PhysMemReadClient#(numeric type asz, numeric type dsz);
   interface Get#(PhysMemRequest#(asz,dsz))    readReq;
   interface Put#(MemData#(dsz)) readData;
endinterface

interface PhysMemWriteClient#(numeric type asz, numeric type dsz);
   interface Get#(PhysMemRequest#(asz,dsz))    writeReq;
   interface Get#(MemData#(dsz)) writeData;
   interface Put#(Bit#(MemTagSize))       writeDone;
endinterface

interface PhysMemReadServer#(numeric type asz, numeric type dsz);
   interface Put#(PhysMemRequest#(asz,dsz)) readReq;
   interface Get#(MemData#(dsz))     readData;
endinterface

interface PhysMemWriteServer#(numeric type asz, numeric type dsz);
   interface Put#(PhysMemRequest#(asz,dsz)) writeReq;
   interface Put#(MemData#(dsz))            writeData;
   interface Get#(Bit#(MemTagSize))         writeDone;
endinterface

//
///////////////////////////////////////////////////////////////////////////////////

instance Connectable#(PhysMemReadClient#(asz,dsz), PhysMemReadServer#(asz,dsz));
   module mkConnection#(PhysMemReadClient#(asz,dsz) source, PhysMemReadServer#(asz,dsz) sink)(Empty);
      rule mr_request;
         let req <- source.readReq.get();
         sink.readReq.put(req);
      endrule
      rule mr_response;
         let resp <- sink.readData.get();
         source.readData.put(resp);
      endrule
   endmodule
endinstance

instance Connectable#(PhysMemWriteClient#(asz,dsz), PhysMemWriteServer#(asz,dsz));
   module mkConnection#(PhysMemWriteClient#(asz,dsz) source, PhysMemWriteServer#(asz,dsz) sink)(Empty);
      rule mw_request;
         let req <- source.writeReq.get();
         sink.writeReq.put(req);
      endrule
      rule mw_response;
         let resp <- source.writeData.get();
         sink.writeData.put(resp);
      endrule
      rule mw_done;
         let resp <- sink.writeDone.get();
         source.writeDone.put(resp);
      endrule
   endmodule
endinstance


instance Connectable#(PhysMemMaster#(addrWidth, busWidth), PhysMemSlave#(addrWidth, busWidth));
   module mkConnection#(PhysMemMaster#(addrWidth, busWidth) m, PhysMemSlave#(addrWidth, busWidth) s)(Empty);
      mkConnection(m.read_client.readReq, s.read_server.readReq);
      mkConnection(s.read_server.readData, m.read_client.readData);
      mkConnection(m.write_client.writeReq, s.write_server.writeReq);
      mkConnection(m.write_client.writeData, s.write_server.writeData);
      mkConnection(s.write_server.writeDone, m.write_client.writeDone);
   endmodule
endinstance

// this is used for debugging MemToPcie/PcieToMem in BsimTop.bsv
instance Connectable#(PhysMemMaster#(32, busWidth), PhysMemSlave#(40, busWidth));
   module mkConnection#(PhysMemMaster#(32, busWidth) m, PhysMemSlave#(40, busWidth) s)(Empty);
      //mkConnection(m.read_client.readReq, s.read_server.readReq);
      rule readreq;
         let req <- m.read_client.readReq.get();
         s.read_server.readReq.put(PhysMemRequest { addr: extend(req.addr), burstLen: req.burstLen, tag: req.tag
`ifdef BYTE_ENABLES
                                                   , firstbe: req.firstbe, lastbe: req.lastbe
`endif
            });
      endrule

      mkConnection(s.read_server.readData, m.read_client.readData);
      //mkConnection(m.write_client.writeReq, s.write_server.writeReq);
      rule writereq;
         let req <- m.write_client.writeReq.get();
         s.write_server.writeReq.put(PhysMemRequest { addr: extend(req.addr), burstLen: req.burstLen, tag: req.tag
`ifdef BYTE_ENABLES
                                                     , firstbe: req.firstbe, lastbe: req.lastbe
`endif
        });
      endrule
      mkConnection(m.write_client.writeData, s.write_server.writeData);
      mkConnection(s.write_server.writeDone, m.write_client.writeDone);
   endmodule
endinstance

function Bool isQuadWordAligned(Bit#(7) lower_addr);
   return (lower_addr[2:0]==3'b0);
endfunction

function Put#(t) null_put();
   return (interface Put;
              method Action put(t x) if (False);
                 noAction;
              endmethod
           endinterface);
endfunction

function Get#(t) null_get();
   return (interface Get;
              method ActionValue#(t) get() if (False);
                 return ?;
              endmethod
           endinterface);
endfunction

function  PhysMemWriteClient#(addrWidth, busWidth) null_phys_mem_write_client();
   return (interface PhysMemWriteClient;
              interface Get writeReq = null_get;
              interface Get writeData = null_get;
              interface Put writeDone = null_put;
           endinterface);
endfunction

function  PhysMemReadClient#(addrWidth, busWidth) null_phys_mem_read_client();
   return (interface PhysMemReadClient;
              interface Get readReq = null_get;
              interface Put readData = null_put;
           endinterface);
endfunction

function  PhysMemWriteServer#(addrWidth, busWidth) null_phys_mem_write_server();
   return (interface PhysMemWriteServer;
              interface Put writeReq = null_put;
              interface Put writeData = null_put;
              interface Get writeDone = null_get;
           endinterface);
endfunction

function  PhysMemReadServer#(addrWidth, busWidth) null_phys_mem_read_server();
   return (interface PhysMemReadServer;
              interface Put readReq = null_put;
              interface Get readData = null_get;
           endinterface);
endfunction

