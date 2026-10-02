def CC_CBC : CallingConv<[
  CCIfType<[i32, i64], CCAssignToReg<[IR1, IR2, IR3, IR4, IR5, IR6]>>,
  CCIfType<[i32, i64], CCAssignToStack<8, 8>>,
  CCIfType<[f32, f64], CCAssignToReg<[FR0, FR1, FR2, FR3, FR4, FR5, FR6, FR7]>>,
  CCIfType<[f32, f64], CCAssignToStack<8, 8>>
]>;

def RetCC_CBC : CallingConv<[
  CCIfType<[i32, i64], CCAssignToReg<[IR1, IR2]>>,
  CCIfType<[f32, f64], CCAssignToReg<[FR0, FR1]>>
]>;
