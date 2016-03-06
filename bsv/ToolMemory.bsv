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

import GetPut::*;
import Vector::*;

//
// Dma channel type
//
typedef enum {
   ChannelType_Read, ChannelType_Write
   } ChannelType deriving (Bits,Eq,FShow);

//
// @brief Channel Identifier
//
//typedef Bit#(16) DmaChannelId;

typedef struct {
   Bit#(32) x;
   Bit#(32) y;
   Bit#(32) z;
   Bit#(32) w;
   } DmaDbgRec deriving(Bits);

typedef enum {
   DmaErrorNone,
   DmaErrorSGLIdOutOfRange_r,
   DmaErrorSGLIdOutOfRange_w,
   DmaErrorOffsetOutOfRange,
   DmaErrorSGLIdInvalid,
   DmaErrorTileTagOutOfRange
   } DmaErrorType deriving (Bits);

//
// @brief Events sent from a Dma engine
//
interface MemServerIndication;
   method Action addrResponse(Bit#(64) physAddr);
   method Action reportStateDbg(DmaDbgRec rec);
   method Action reportMemoryTraffic(Bit#(64) words);
   method Action error(Bit#(32) code, Bit#(64) offset, Bit#(64) extra);
endinterface

typedef Bit#(32) SpecialTypeForSendingFd;

typedef enum {
   Idle, Stopped, Running
   } TileState deriving (Bits,Eq);

typedef struct {
   Bit#(2) tile;
   TileState state;
   } TileControl deriving (Bits);

//
// @brief Control interface to Dma engine
//
interface MemServerRequest;
   // @brief Changes tile status
   //
   method Action setTileState(TileControl tc);
   //
   // @brief Requests debug info for the specified channel type
   //
   method Action stateDbg(ChannelType rc);
   method Action memoryTraffic(ChannelType rc);
endinterface
