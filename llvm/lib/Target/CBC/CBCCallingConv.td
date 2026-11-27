// Argument and result locations of CBC (ClangCBC, MethodAbiKind::C) functions.
// The engine places arguments by signature order (cbc-engine PlatformTraits), so
// these tables mirror IR_HEAD_AREA / FR_HEAD_AREA of the host flavour. The `sret`
// pointer is a plain first integer argument in both flavours (the engine does not
// set the SRET flag for C methods, agents/01 section 3).

// x86-64 flavour: IR1..IR6, stack arguments through the tail register IR7.
def CC_CBC_X64 : CallingConv<[
  CCIfType<[i32, i64], CCAssignToReg<[IR1, IR2, IR3, IR4, IR5, IR6]>>,
  CCIfType<[i32, i64], CCAssignToStack<8, 8>>,
  CCIfType<[f32, f64], CCAssignToReg<[FR0, FR1, FR2, FR3, FR4, FR5, FR6, FR7]>>,
  CCIfType<[f32, f64], CCAssignToStack<8, 8>>
]>;

// AArch64 flavour: IR1..IR8 (x0-x7), stack arguments through the tail register IR10.
def CC_CBC_A64 : CallingConv<[
  CCIfType<[i32, i64], CCAssignToReg<[IR1, IR2, IR3, IR4, IR5, IR6, IR7, IR8]>>,
  CCIfType<[i32, i64], CCAssignToStack<8, 8>>,
  CCIfType<[f32, f64], CCAssignToReg<[FR0, FR1, FR2, FR3, FR4, FR5, FR6, FR7]>>,
  CCIfType<[f32, f64], CCAssignToStack<8, 8>>
]>;

// Results are the same for both flavours. Aggregates that do not fit (for example
// an HFA of three or four members) are demoted to a hidden sret pointer by
// CanLowerReturn, consistently on both sides of a CBC call.
def RetCC_CBC : CallingConv<[
  CCIfType<[i32, i64], CCAssignToReg<[IR1, IR2]>>,
  CCIfType<[f32, f64], CCAssignToReg<[FR0, FR1]>>
]>;
