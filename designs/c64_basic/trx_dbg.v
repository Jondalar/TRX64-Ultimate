// SPDX-License-Identifier: GPL-3.0-or-later
// The C64 side of the debug hardware (T21, ARCH-lean-state-access §7.1/§7.2),
// in c64_core's clk next to the gate: instruction-boundary and cycle stops,
// steps, counters, the exec bitmap, two watch units, the stop record. The
// drive's comparator and step are drv_dbg.v (the drive's clock).
//
// Every stop source is evaluated in the clk where a phi2 falling edge is about
// to pass the gate (edge = ce && phi2n) and closes the gate on that same edge
// (brk): no stop is late. An instruction boundary (IB) is the edge that ends an
// opcode fetch the CPU completes (6502N sync, RDY high, the CPU on the bus)
// with no interrupt decided (intg low): after it the opcode is in IR and
// nothing else of the instruction has run. IPC is the address of that fetch.
//
// Watch units see the bus and the CPU as they are in the cycle that ends at
// the edge. A register qualifier therefore sees A/X/Y/P (and S after TXS)
// before the write-back that the previous instruction makes on an IB edge:
// exact for the access and cycle types and for S on IB types after stack
// instructions (n: S >= S0 at PC+3; ret: PREV_RET and S > S0); for A/X/Y/P on
// the IB type the app checks the condition after the stop (soft).
//
// Registers (32-bit words, trx_sys.v rules; r_addr = {page 3, word index}):
//   page 0
//     118  OWNER     W   b0 the debugger owns run control: the app's C64_STOP no
//                        longer stops the machine or its CPU (top.v)
//     120  CYCLES    R   phi2 cycles the machine ran (steps and bus DMA cycles
//                        included), bits 31:0; reading it latches bits 47:32
//     124  CYCLES_HI R   bits 47:32 as latched by the last read of 120
//     128  INSTRS    R   instruction boundaries passed
//     130  CYC_CMP   W   stop on the edge that ends cycle CYC_CMP: the one that
//                        passes while CYCLES[31:0] equals it (after the stop
//                        CYCLES = CYC_CMP + 1; writing it arms it; once)
//     134  CYC_CMP_EN W  b31 armed (0 disarms)
//   page 3
//     000  CMD       W   1 PAUSE_CB (at the next phi2 falling edge), 2 PAUSE_IB
//                        (at the next IB), 3 RUN (release the debugger's hold
//                        and the drive's; the gate then waits for frame
//                        alignment as every resume), 5 STEP_INSTR, 6
//                        STEP_CYCLES (ARG times; only while the gate is shut;
//                        releases a CPU held by bus DMA), 0x20 CLEAR_STOP
//                        (STOP_NEW to 0; RUN and the steps, the drive's too,
//                        clear it). RUN, 5 and 6 are requests (STATUS b12):
//                        STOP_NEW clears at once, the hold is released or the
//                        step started in the next clock;
//                        0x10-0x12 are the drive's (drv_dbg.v)
//     004  ARG       W   b15:0 count for the steps (0 = 1)
//     008  STATUS    R   b0 the gate is shut, b1 a debugger stop is in force (the signal
//                        ultimate_sys opens the app window's D page for), b2 the last edge was an IB, b3
//                        STOP_NEW (a stop record waits), b4 the CPU is held
//                        (RDY) by bus DMA, b5 the app's C64_STOP is set, b6 the
//                        debugger's hold is off but the gate is still shut
//                        (resume waits for frame alignment), b7 a step runs or
//                        is requested, b12 RUN or a step is requested (one clock),
//                        b11:8 REASON of the record, b16/b17 watch unit 0/1
//                        enabled, b18 exec bitmap on, b19 CYC_CMP armed, b20
//                        OWNER, b31:24 stops (wraps)
//     010  STOP0     R   stop record, written at a stop while STOP_NEW is 0:
//                        {POS, DATA, FLAGS 3, UNIT 1, REASON 4}. REASON 1 PAUSE,
//                        2 step done, 3 exec bitmap, 5 watch unit (address
//                        source), 6 watch unit (raster source), 7 CYC_CMP, 8 the
//                        drive comparator (the C64 followed the drive), 9 a drive
//                        step ended. FLAGS b0 the stop edge is an IB, b1 the
//                        drive stopped first, b2 the step released a CPU held by
//                        bus DMA. POS the raster position {line 9, cycle in the
//                        line 7}. CYCLES at the stop: read CYCLES before any
//                        further DMA or step
//     014  STOP1     R   {ADDR (the bus address of the edge), IPC}
//     018  IPC_SET   W   {7'd0, IB b24, opcode b23:16, IPC b15:0}: the reported
//                        PC, its opcode and STATUS b2 after the app moved the
//                        CPU without an edge of its own: a redirect (PC and the
//                        fetched byte loaded through the staging, c64_core state
//                        bus chip 1 w8-w14) or a ring restore that landed on a
//                        frame's snap (T21 §5.1)
//     040  BMP_IDX   W   b10:0 word of the exec bitmap (32 addresses each)
//     044  BMP_DATA  RW  the 32 bits at BMP_IDX (bit n = address 32 * idx + n);
//                        reading or writing it increments BMP_IDX
//     048  DBG_CTRL  W   b0 the exec bitmap stops at an IB whose IPC has its bit
//     080  + 0x20 * n    watch unit n (0, 1), write-only but HITS:
//       +00 CTRL         b0 enable, b3:1 type (1 IB, 2 read, 3 write, 4 read or
//                        write (opcode fetches are not reads here), 5 every phi2
//                        cycle), b4 source (0 the CPU's address, 1 the raster
//                        position {line, cycle}), b6:5 action (0 stop, 1 count,
//                        2 log: a record into engine stream 1), b8 one-shot
//                        (the enable clears with the action), b9 chain (hits
//                        only when the other unit matches on the same edge),
//                        b14 PREV_RET (the instruction before the IB was RTS or
//                        RTI)
//       +04 RANGE        {HI, LO}: LO <= value <= HI (one address: HI = LO)
//       +08 DATA         {DMASK b15:8, DVAL b7:0}: (data ^ DVAL) & DMASK == 0
//                        (data = the byte on the bus; on an IB the opcode)
//       +0C REG          {RMASK b31:24, RVAL b23:16, ROP b9:8 (0 ==, 1 !=, 2 <,
//                        3 >=, unsigned), RSEL b2:0 (0 none, 1 A, 2 X, 3 Y, 4 S,
//                        5 P)}: (reg & RMASK) ROP RVAL
//       +10 PASS         b15:0 the action starts with hit PASS + 1 (HITS
//                        counts from 0: clear HITS with PASS)
//       +14 HITS     W   clears this unit's HITS
//     0B4  HITS      R   {unit 1, unit 0}: hits so far, 16 bits each (passed
//                        ones too; they wrap)
// Log record (stream 1, space 8): [15:0] ADDR, [23:16] DATA, [27:24] {0, IB,
// write, unit}, [31:28] 8, [47:32] IPC, [62:48] CYCLES low 15 (before the edge).
module trx_dbg (
    input  wire        clk,
    input  wire        rst,
    // the gate and the bus (c64_core.v): edge = this clk's gclk edge is a
    // phi2 falling edge the gate lets through
    input  wire        ce,
    input  wire        phi2n,
    input  wire        halted,
    input  wire        held,
    input  wire [15:0] addr,
    input  wire  [7:0] data,
    input  wire        we,
    input  wire        sync,
    input  wire        intg,
    input  wire        rdy,
    input  wire        cpu_bus,             // the CPU is the bus master (not REU, not DMA)
    input  wire [39:0] regs,                // {P, S, Y, X, A}
    input  wire [15:0] pos,                 // {raster line 9, cycle in the line 7}
    input  wire        fw_stop,             // the app's C64_STOP
    output wire        armed,               // a stop source that needs the exact machine is armed (T29)
    input  wire        drv_stop,            // pulse: the drive comparator stopped the drive
    input  wire        drv_done,            // pulse: a drive step ended
    // to the gate
    output reg         hold = 1'b0,         // the debugger holds the machine
    output wire        stop,                // a debugger stop is in force (hold, gate shut, no step)
    output reg         rel = 1'b0,          // RUN or a step is requested, taken in the next clock
    output wire        brk,                 // close the gate on this edge
    output wire        keep,                // a step keeps the gate open on this edge
    output wire        go,                  // open the gate for a step
    output reg         own = 1'b0,
    output reg         ev = 1'b0,           // a stop (record or not)
    output reg  [15:0] ipc = 16'd0,
    output reg   [7:0] op = 8'd0,           // the opcode at IPC
    output reg         at_ib = 1'b0,
    // the trace former's feed (trx_engine.v, T21 §4.11.1): an IB one clock later
    // (ipc and op are then the fetch's), and the bus data of the two CPU cycles
    // after the fetch (they hold the last instruction's until the next one's)
    output reg         tr_ib = 1'b0,
    output reg   [7:0] tr_b1 = 8'd0,
    output reg   [7:0] tr_b2 = 8'd0,
    // log action: a record for the engine's stream 1 (this clk)
    output wire        log_put,
    output wire [63:0] log_rec,
    // registers
    input  wire        r_we,
    input  wire        r_re,
    input  wire [10:0] r_addr,              // {page 3, word}
    input  wire [31:0] r_wdata,
    output reg  [31:0] r_rdata
);
    wire edge_ = ce && phi2n;
    wire ib    = edge_ && sync && !intg && rdy && cpu_bus;
    wire cyc   = edge_ && cpu_bus && (we || rdy);    // a CPU access that completes
    wire wr    = r_we;
    wire [7:0] cmd = r_wdata[7:0];
    wire wcmd  = wr && r_addr == 11'h400;

    // ---- counters ----------------------------------------------------------------------
    // the two long counters may go into DSP48 adders (Vivado; 2 of 120 DSPs)
    (* use_dsp = "yes" *) reg [47:0] cycles = 48'd0;
    (* use_dsp = "yes" *) reg [31:0] instrs = 32'd0;
    reg  [15:0] cyc_hi = 16'd0;
    reg  [31:0] cyc_cmp = 32'd0;
    reg         cmp_en = 1'b0;
    wire        cmp_hit = edge_ && cmp_en && cycles[31:0] == cyc_cmp;

    // ---- the exec bitmap: 2048 x 32, two block RAMs ---------------------------------
    (* ram_style = "block" *) reg [31:0] bmp [0:2047];
    reg  [31:0] bmp_q, bmp_rq;
    reg   [4:0] bmp_bit = 5'd0;
    reg  [10:0] bmp_idx = 11'd0;
    reg         bmp_en = 1'b0;
    wire        bmp_wr = wr && r_addr == 11'h411;
    always @(posedge clk) begin
        bmp_q   <= bmp[addr[15:5]];
        bmp_bit <= addr[4:0];
    end
    always @(posedge clk) begin
        if (bmp_wr) bmp[bmp_idx] <= r_wdata;
        bmp_rq <= bmp[bmp_idx];
    end
    wire bmp_hit = ib && bmp_en && bmp_q[bmp_bit];

    // ---- watch units -------------------------------------------------------------------
    wire        prev_ret = op == 8'h60 || op == 8'h40;  // op: the opcode of the IB before
    wire  [1:0] m, en, chain, src, pass_ok;
    wire  [1:0] act0, act1;
    wire [15:0] hits0, hits1;
    wire  [1:0] hit   = {m[1] && (!chain[1] || m[0]), m[0] && (!chain[0] || m[1])};
    wire  [1:0] fire  = hit & pass_ok;
    wire  [1:0] ustop = fire & {act1 == 2'd0, act0 == 2'd0};
    wire  [1:0] ulog  = fire & {act1 == 2'd2, act0 == 2'd2};
    genvar gu;
    generate
        for (gu = 0; gu < 2; gu = gu + 1) begin : g_u
            reg  [15:0] ctrl = 16'd0, lo = 16'd0, hi = 16'd0, pass = 16'd0;
            reg   [7:0] dval = 8'd0, dmask = 8'd0, rval = 8'd0, rmask = 8'd0;
            reg   [2:0] rsel = 3'd0;
            reg   [1:0] rop = 2'd0;
            reg  [15:0] hits = 16'd0;
            wire        wu = wr && r_addr[10:3] == {1'b1, 6'h02, gu[0]};
            always @(posedge clk) begin
                if (wu && r_addr[2:0] == 3'd0) ctrl <= r_wdata[15:0];
                if (wu && r_addr[2:0] == 3'd1) begin lo <= r_wdata[15:0]; hi <= r_wdata[31:16]; end
                if (wu && r_addr[2:0] == 3'd2) begin dval <= r_wdata[7:0]; dmask <= r_wdata[15:8]; end
                if (wu && r_addr[2:0] == 3'd3) begin
                    rsel <= r_wdata[2:0]; rop <= r_wdata[9:8]; rval <= r_wdata[23:16]; rmask <= r_wdata[31:24];
                end
                if (wu && r_addr[2:0] == 3'd4) pass <= r_wdata[15:0];
                if (wu && r_addr[2:0] == 3'd5) hits <= 16'd0;
                else if (hit[gu]) hits <= hits + 1'b1;
                if (fire[gu] && ctrl[8]) ctrl[0] <= 1'b0;
                if (rst) ctrl[0] <= 1'b0;
            end
            wire [15:0] v = ctrl[4] ? pos : addr;
            reg   [7:0] rv;
            always @(*)
                case (rsel)
                3'd1: rv = regs[7:0];
                3'd2: rv = regs[15:8];
                3'd3: rv = regs[23:16];
                3'd4: rv = regs[31:24];
                3'd5: rv = regs[39:32];
                default: rv = 8'd0;
                endcase
            wire [7:0] rm = rv & rmask;
            wire       req = rm == rval, rge = rm >= rval;
            wire       rq = rsel == 3'd0 || (rop[1] ? (rop[0] ? rge : !rge) : (rop[0] ? !req : req));
            wire       tq = ctrl[3:1] == 3'd1 ? ib :
                            ctrl[3:1] == 3'd2 ? cyc && !we && !sync :
                            ctrl[3:1] == 3'd3 ? cyc && we :
                            ctrl[3:1] == 3'd4 ? cyc && !sync :
                            ctrl[3:1] == 3'd5 ? edge_ : 1'b0;
            assign m[gu] = ctrl[0] && tq && v >= lo && v <= hi &&
                           ((data ^ dval) & dmask) == 8'd0 && rq && (!ctrl[14] || prev_ret);
            assign en[gu]      = ctrl[0];
            assign chain[gu]   = ctrl[9];
            assign src[gu]     = ctrl[4];
            assign pass_ok[gu] = hits >= pass;
            if (gu == 0) begin : g_0
                assign act0  = ctrl[6:5];
                assign hits0 = hits;
            end else begin : g_1
                assign act1  = ctrl[6:5];
                assign hits1 = hits;
            end
        end
    endgenerate
    assign log_put = ulog != 2'd0;
    assign log_rec = {1'b0, cycles[14:0], ib ? addr : ipc, 4'd8, 1'b0, ib, we, !ulog[0], data, addr};

    // ---- run control -------------------------------------------------------------------
    reg         stepping = 1'b0, started = 1'b0, step_ins = 1'b0;
    reg  [15:0] step_n = 16'd0;
    reg  [15:0] arg = 16'd0;
    reg         pend_ib = 1'b0, pend_cb = 1'b0, pend_drv = 1'b0;
    reg         rel_step = 1'b0, rel_ins = 1'b0;   // the request is a step (of instructions)
    wire        counted  = started && edge_ && (!step_ins || ib);
    wire        step_end = stepping && counted && step_n == 16'd1;
    assign armed = stepping || pend_ib || pend_cb || pend_drv || bmp_en || cmp_en || en != 2'b00;
    assign keep = stepping && !step_end;
    assign go   = stepping && !started;
    wire        pause_ib = pend_ib && ib;
    wire        shut     = halted && !ce;
    // a debugger stop: the hold is on, the gate is shut and no step runs. The gate's
    // own halted_r, not ce: a bus cycle of the app's window or the lens pulses ce
    // and is no end of the stop.
    assign stop = hold && halted && !stepping;
    assign brk = ustop != 2'd0 || bmp_hit || cmp_hit || step_end || pause_ib || (pend_drv && edge_);
    wire        rec = brk || (pend_cb && edge_);  // an edge that ends in a stop

    // ---- stop record -------------------------------------------------------------------
    reg         stop_new = 1'b0;
    reg   [7:0] stops = 8'd0;
    reg   [3:0] s_reason = 4'd0;
    reg         s_unit = 1'b0;
    reg   [2:0] s_flags = 3'd0;
    reg   [7:0] s_data = 8'd0;
    reg  [15:0] s_addr = 16'd0, s_ipc = 16'd0, s_pos = 16'd0;
    reg         held_go = 1'b0;             // the running step released a held CPU
    wire  [3:0] reason = ustop[0] ? (src[0] ? 4'd6 : 4'd5) :
                         ustop[1] ? (src[1] ? 4'd6 : 4'd5) :
                         bmp_hit ? 4'd3 : cmp_hit ? 4'd7 : pend_drv ? 4'd8 :
                         step_end ? 4'd2 : 4'd1;
    wire        now_drv  = drv_stop && shut;  // the drive stopped while the machine was
    wire        new_rec  = !stop_new && (rec || now_drv || drv_done);

    reg   [1:0] tr_n = 2'd0;                // CPU cycles to come that tr_b1/tr_b2 take
    always @(posedge clk) begin
        tr_ib <= ib;
        if (ib) tr_n <= 2'd1;
        else if (cyc && tr_n == 2'd1) begin tr_b1 <= data; tr_n <= 2'd2; end
        else if (cyc && tr_n == 2'd2) begin tr_b2 <= data; tr_n <= 2'd0; end
    end
    always @(posedge clk) begin
        ev <= rec || now_drv || drv_done || (wcmd && cmd == 8'h01 && shut);
        if (edge_) cycles <= cycles + 1'b1;
        if (ib) begin
            instrs <= instrs + 1'b1;
            ipc    <= addr;
            op     <= data;
        end else if (wr && r_addr == 11'h406) begin
            ipc    <= r_wdata[15:0];
            op     <= r_wdata[23:16];
        end
        if (edge_) at_ib <= ib;
        else if (wr && r_addr == 11'h406) at_ib <= r_wdata[24];
        if (cmp_hit) cmp_en <= 1'b0;

        // steps
        if (stepping && ce) started <= 1'b1;
        if (stepping && counted && step_n != 16'd1) step_n <= step_n - 1'b1;
        if (pause_ib) pend_ib <= 1'b0;
        if (pend_cb && edge_) pend_cb <= 1'b0;
        if (brk) begin hold <= 1'b1; stepping <= 1'b0; pend_drv <= 1'b0; end
        if (drv_stop) begin hold <= 1'b1; pend_drv <= !shut; end

        // the record: the first stop after STOP_NEW was cleared
        if (rec || now_drv || drv_done) stops <= stops + 1'b1;
        if (new_rec) begin
            stop_new <= 1'b1;
            s_reason <= rec ? reason : now_drv ? 4'd8 : 4'd9;
            s_unit   <= ustop[1] && !ustop[0];
            s_flags  <= {held_go, pend_drv || now_drv, ib};
            s_data   <= data;
            s_addr   <= addr;
            s_ipc    <= ib ? addr : ipc;
            s_pos    <= pos;
        end

        // registers
        if (r_re && r_addr == 11'h048) cyc_hi <= cycles[47:32];
        if (r_re && r_addr == 11'h411) bmp_idx <= bmp_idx + 1'b1;
        if (bmp_wr) bmp_idx <= bmp_idx + 1'b1;
        if (wr && r_addr == 11'h046) own <= r_wdata[0];
        if (wr && r_addr == 11'h04C) begin cyc_cmp <= r_wdata; cmp_en <= 1'b1; end
        if (wr && r_addr == 11'h04D) cmp_en <= r_wdata[31];
        if (wr && r_addr == 11'h401) arg <= r_wdata[15:0];
        if (wr && r_addr == 11'h410) bmp_idx <= r_wdata[10:0];
        if (wr && r_addr == 11'h412) bmp_en <= r_wdata[0];
        if (wcmd)
            case (cmd)
            8'h01: begin
                hold <= 1'b1;
                if (shut) begin
                    if (!stop_new) begin stop_new <= 1'b1; s_reason <= 4'd1; end
                end else
                    pend_cb <= 1'b1;
            end
            8'h02: pend_ib <= 1'b1;
            8'h03: begin                        // RUN: a request
                rel      <= 1'b1;
                rel_step <= 1'b0;
                stop_new <= 1'b0;
            end
            8'h05, 8'h06: if (shut && !stepping) begin
                rel      <= 1'b1;               // a step: a request too
                rel_step <= 1'b1;
                rel_ins  <= cmd == 8'h05;
                stop_new <= 1'b0;
            end
            8'h12, 8'h20: stop_new <= 1'b0;     // a drive step's end makes a record too
            default: ;
            endcase

        // the request is taken: RUN lets go, a step opens the gate
        if (rel) begin
            rel <= 1'b0;
            if (rel_step) begin
                stepping <= 1'b1;
                started  <= 1'b0;
                step_ins <= rel_ins;
                step_n   <= arg == 16'd0 ? 16'd1 : arg;
                held_go  <= held;
                hold     <= 1'b1;
            end else begin
                hold <= 1'b0; stepping <= 1'b0; pend_ib <= 1'b0; pend_cb <= 1'b0;
                pend_drv <= 1'b0;
            end
        end

        if (rst) begin
            hold <= 1'b0; stepping <= 1'b0; pend_ib <= 1'b0; pend_cb <= 1'b0; pend_drv <= 1'b0; rel <= 1'b0;
            bmp_en <= 1'b0; cmp_en <= 1'b0;
        end
    end

    // ---- read side (combinational from r_addr) ----------------------------------------
    always @(*) begin
        r_rdata = 32'd0;
        case (r_addr)
        11'h048: r_rdata = cycles[31:0];
        11'h049: r_rdata = {16'd0, cyc_hi};
        11'h04A: r_rdata = instrs;
        11'h402: r_rdata = {stops, 3'd0, own, cmp_en, bmp_en, en, 3'd0, rel, s_reason, stepping | (rel & rel_step),
                            !hold && halted, fw_stop, held, stop_new, at_ib, stop, shut};
        11'h404: r_rdata = {s_pos, s_data, s_flags, s_unit, s_reason};
        11'h405: r_rdata = {s_addr, s_ipc};
        11'h411: r_rdata = bmp_rq;
        11'h42D: r_rdata = {hits1, hits0};
        default: ;
        endcase
    end
endmodule
