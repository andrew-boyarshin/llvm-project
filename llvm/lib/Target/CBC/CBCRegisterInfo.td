class CBCReg<bits<16> enc, string n> : Register<n> {
  let HWEncoding = enc;
  let Namespace = "CBC";
}

def IRZ : CBCReg<0, "irz">;
foreach I = 1-13 in
  def IR#I : CBCReg<I, "ir"#I>;
def IRACC : CBCReg<14, "iracc">;
foreach I = 0-15 in
  def FR#I : CBCReg<I, "fr"#I>;

def GPR : RegisterClass<"CBC", [i64, i32], 64,
  (add IR1, IR2, IR3, IR4, IR5, IR6, IR7, IR8, IR9, IR10, IR11, IR12, IR13, IRACC)>;
def FPR : RegisterClass<"CBC", [f64, f32], 64, (sequence "FR%u", 0, 15)>;
