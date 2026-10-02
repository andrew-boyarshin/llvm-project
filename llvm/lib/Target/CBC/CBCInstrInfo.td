class CBCInst<dag outs, dag ins, string asmstr> : Instruction {
  let Namespace = "CBC";
  let OutOperandList = outs;
  let InOperandList = ins;
  let AsmString = asmstr;
  let Size = 1;
}

def MOV64rr : CBCInst<(outs GPR:$d), (ins GPR:$s), "mov.W64 $d, $s">;
def MOV64ri : CBCInst<(outs GPR:$d), (ins i64imm:$imm), "mov.W64 $d, $imm">;
def FMOV64i : CBCInst<(outs FPR:$d), (ins i64imm:$imm), "fmov.W64 $d, $imm">;
def FMOV32i : CBCInst<(outs FPR:$d), (ins i64imm:$imm), "fmov.W32 $d, $imm">;

def ADD32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "add.w32 $d, $l, $r">;
def ADD64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "add.w64 $d, $l, $r">;
def ADD32ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "addi.w32 $d, $l, $imm">;
def ADD64ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "addi.w64 $d, $l, $imm">;
def SUB32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "sub.w32 $d, $l, $r">;
def SUB64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "sub.w64 $d, $l, $r">;
def AND32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "and.w32 $d, $l, $r">;
def AND32ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "andi.w32 $d, $l, $imm">;
def AND64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "and.w64 $d, $l, $r">;
def AND64ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "andi.w64 $d, $l, $imm">;
def OR32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "or.w32 $d, $l, $r">;
def OR64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "or.w64 $d, $l, $r">;
def XOR32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "xor.w32 $d, $l, $r">;
def XOR64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "xor.w64 $d, $l, $r">;
def MUL32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "mul.w32 $d, $l, $r">;
def MUL32ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "muli.w32 $d, $l, $imm">;
def MUL64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "mul.w64 $d, $l, $r">;
def MUL64ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "muli.w64 $d, $l, $imm">;
def SHL32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "lsl.w32 $d, $l, $r">;
def SHL32ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "lsli.w32 $d, $l, $imm">;
def SHL64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "lsl.w64 $d, $l, $r">;
def SHL64ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "lsli.w64 $d, $l, $imm">;
def SRL32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "lsr.w32 $d, $l, $r">;
def SRL32ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "lsri.w32 $d, $l, $imm">;
def SRL64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "lsr.w64 $d, $l, $r">;
def SRL64ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "lsri.w64 $d, $l, $imm">;
def SRA32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "asr.w32 $d, $l, $r">;
def SRA32ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "asri.w32 $d, $l, $imm">;
def SRA64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "asr.w64 $d, $l, $r">;
def SRA64ri : CBCInst<(outs GPR:$d), (ins GPR:$l, i64imm:$imm), "asri.w64 $d, $l, $imm">;
def SDIV64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "sdiv.w64 $d, $l, $r">;
def SREM64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "srem.w64 $d, $l, $r">;
def UDIV64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "udiv.w64 $d, $l, $r">;
def UREM64rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "urem.w64 $d, $l, $r">;
def SDIV32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "sdiv.w32 $d, $l, $r">;
def SREM32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "srem.w32 $d, $l, $r">;
def UDIV32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "udiv.w32 $d, $l, $r">;
def UREM32rr : CBCInst<(outs GPR:$d), (ins GPR:$l, GPR:$r), "urem.w32 $d, $l, $r">;
def FMOV64rr : CBCInst<(outs FPR:$d), (ins FPR:$s), "fmov.W64 $d, $s">;
def FBIN64 : CBCInst<(outs FPR:$d), (ins i32imm:$op, FPR:$l, FPR:$r), "fbin.w64 $d, $l, $r">;
def FBIN32 : CBCInst<(outs FPR:$d), (ins i32imm:$op, FPR:$l, FPR:$r), "fbin.w32 $d, $l, $r">;
def FUNARY64 : CBCInst<(outs FPR:$d), (ins i32imm:$op, FPR:$s), "funary.w64 $d, $s">;
def FUNARY32 : CBCInst<(outs FPR:$d), (ins i32imm:$op, FPR:$s), "funary.w32 $d, $s">;
def I2F : CBCInst<(outs FPR:$d), (ins GPR:$s), "i2f $d, $s">;
def F2I : CBCInst<(outs GPR:$d), (ins FPR:$s), "f2i $d, $s">;
def CVT_IF : CBCInst<(outs FPR:$d), (ins i32imm:$to, i32imm:$from, GPR:$s), "cvt.if $d, $s">;
def CVT_FI : CBCInst<(outs GPR:$d), (ins i32imm:$to, i32imm:$from, FPR:$s), "cvt.fi $d, $s">;
def CVT_FF : CBCInst<(outs FPR:$d), (ins i32imm:$to, i32imm:$from, FPR:$s), "cvt.ff $d, $s">;

def JMP : CBCInst<(outs), (ins variable_ops), "jump"> {
  let isBranch = 1;
  let isTerminator = 1;
  let isBarrier = 1;
}
def BCC64 : CBCInst<(outs), (ins i32imm:$cc, GPR:$l, GPR:$r, variable_ops), "bcc64 $l, $r"> {
  let isBranch = 1;
  let isTerminator = 1;
}
def BCCF : CBCInst<(outs), (ins i32imm:$cc, FPR:$l, FPR:$r, i32imm:$f32, variable_ops), "bccf $l, $r"> {
  let isBranch = 1;
  let isTerminator = 1;
}
def SCC64 : CBCInst<(outs GPR:$d), (ins i32imm:$cc, GPR:$l, GPR:$r), "scc64 $d, $l, $r">;
def SCCF : CBCInst<(outs GPR:$d), (ins i32imm:$cc, FPR:$l, FPR:$r, i32imm:$f32), "sccf $d, $l, $r">;

def LDRAW8 : CBCInst<(outs GPR:$d), (ins GPR:$b, i64imm:$disp), "ld.raw.mem.8 $d, $b, $disp">;
def LDRAW32 : CBCInst<(outs GPR:$d), (ins GPR:$b, i64imm:$disp), "ld.raw.mem.32 $d, $b, $disp">;
def LDRAW64 : CBCInst<(outs GPR:$d), (ins GPR:$b, i64imm:$disp), "ld.raw.mem.64 $d, $b, $disp">;
def LDRAWF32 : CBCInst<(outs FPR:$d), (ins GPR:$b, i64imm:$disp), "ld.raw.mem.f32 $d, $b, $disp">;
def LDRAWF64 : CBCInst<(outs FPR:$d), (ins GPR:$b, i64imm:$disp), "ld.raw.mem.f64 $d, $b, $disp">;
def CVT_F32_F64 : CBCInst<(outs FPR:$d), (ins FPR:$s), "cvt.f64.f32 $d, $s">;
def STRAW8 : CBCInst<(outs), (ins GPR:$s, GPR:$b, i64imm:$disp), "st.raw.mem.8 $s, $b, $disp">;
def STRAW32 : CBCInst<(outs), (ins GPR:$s, GPR:$b, i64imm:$disp), "st.raw.mem.32 $s, $b, $disp">;
def STRAW64 : CBCInst<(outs), (ins GPR:$s, GPR:$b, i64imm:$disp), "st.raw.mem.64 $s, $b, $disp">;
def STRAWF32 : CBCInst<(outs), (ins FPR:$s, GPR:$b, i64imm:$disp), "st.raw.mem.f32 $s, $b, $disp">;
def STRAWF64 : CBCInst<(outs), (ins FPR:$s, GPR:$b, i64imm:$disp), "st.raw.mem.f64 $s, $b, $disp">;

def LEA_IMAGE : CBCInst<(outs GPR:$d, GPR:$scratch), (ins), "lea.s $d, $scratch, @image"> {
  let isAsCheapAsAMove = 1;
  let isReMaterializable = 1;
}

def LD_FNPTR : CBCInst<(outs GPR:$d), (ins variable_ops), "ld.fnptr $d">;
def STU64 : CBCInst<(outs), (ins GPR:$s, i64imm:$slot), "st.frame $s, $slot">;
def LDU64 : CBCInst<(outs GPR:$d), (ins i64imm:$slot), "ld.frame $d, $slot">;
def STUF64 : CBCInst<(outs), (ins FPR:$s, i64imm:$slot), "st.frame.f64 $s, $slot">;
def LDUF64 : CBCInst<(outs FPR:$d), (ins i64imm:$slot), "ld.frame.f64 $d, $slot">;
def LEA_FRAME : CBCInst<(outs GPR:$d), (ins i64imm:$disp), "lea.frame $d, $disp">;
def ALLOCA : CBCInst<(outs GPR:$d), (ins GPR:$sz, i32imm:$align),
                     "alloca $d, $sz, $align"> {
  let hasSideEffects = 1;
}
def STACKSAVE : CBCInst<(outs GPR:$d), (ins), "stacksave $d"> {
  let hasSideEffects = 1;
}
def STACKRESTORE : CBCInst<(outs), (ins GPR:$s), "stackrestore $s"> {
  let hasSideEffects = 1;
}
def LD_FCB : CBCInst<(outs GPR:$d), (ins), "ld.fcb $d"> {
  let hasSideEffects = 1;
}
def NULLCHECK : CBCInst<(outs), (ins), "nullcheck irz"> {
  let hasSideEffects = 1;
}

def CALL_INDIRECT : CBCInst<(outs), (ins variable_ops), "call.indirect"> {
  let isCall = 1;
  let hasSideEffects = 1;
  let hasPostISelHook = 1;
}

def CALL : CBCInst<(outs), (ins variable_ops), "call.direct"> {
  let isCall = 1;
  let hasSideEffects = 1;
  let hasPostISelHook = 1;
}

def SELECT_GPR : CBCInst<(outs GPR:$d),
    (ins i32imm:$cc, GPR:$l, GPR:$r, GPR:$t, GPR:$f), "select $d, $l, $r"> {
  let usesCustomInserter = 1;
}
def SELECT_FPR : CBCInst<(outs FPR:$d),
    (ins i32imm:$cc, FPR:$l, FPR:$r, FPR:$t, FPR:$f, i32imm:$f32), "selectf $d, $l, $r"> {
  let usesCustomInserter = 1;
}
def SELECT_FPR_I : CBCInst<(outs FPR:$d),
    (ins i32imm:$cc, GPR:$l, GPR:$r, FPR:$t, FPR:$f), "selectfi $d, $l, $r"> {
  let usesCustomInserter = 1;
}

def RET : CBCInst<(outs), (ins), "ret.W64 ir1"> {
  let isTerminator = 1;
  let isReturn = 1;
  let isBarrier = 1;
  let Uses = [IR1];
}
def FRET : CBCInst<(outs), (ins), "fret.W64 fr0"> {
  let isTerminator = 1;
  let isReturn = 1;
  let isBarrier = 1;
  let Uses = [FR0];
}
