#include "CBCFileWriter.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>

using namespace llvm;

static void appendULEB(std::string &O, uint64_t N) {
  while (true) {
    uint8_t B = N & 0x7F;
    N >>= 7;
    if (N)
      O.push_back(static_cast<char>(B | 0x80));
    else {
      O.push_back(static_cast<char>(B));
      return;
    }
  }
}

static void appendU32(std::string &O, uint32_t N) {
  for (int I = 0; I < 4; ++I)
    O.push_back(static_cast<char>((N >> (8 * I)) & 0xFF));
}

static void appendU16(std::string &O, uint16_t N) {
  O.push_back(static_cast<char>(N & 0xFF));
  O.push_back(static_cast<char>((N >> 8) & 0xFF));
}

static std::string ulebStr(uint64_t N) {
  std::string S;
  appendULEB(S, N);
  return S;
}

static std::string cbcString(StringRef S) {
  std::string O = ulebStr(S.size());
  O.append(S.begin(), S.end());
  return O;
}

static uint32_t nameHash(StringRef S) {
  uint32_t H = 0;
  for (unsigned char B : S)
    H = ((H << 5) - H + B) & 0xFFFFFFFFu;
  return H;
}

/// One bucket holding every entry. Valid for any key: hash % 1 == 0.
static std::string hashTable(ArrayRef<uint32_t> Entries) {
  std::string O;
  if (Entries.empty()) {
    appendU32(O, 1);
    appendU32(O, 0);
    appendU32(O, 0);
    return O;
  }
  appendU32(O, 2);
  appendU32(O, Entries.size());
  appendU32(O, 0);
  appendU32(O, Entries.size());
  for (uint32_t E : Entries)
    appendU32(O, E);
  (void)nameHash;
  return O;
}

static void appendSLEB(std::string &O, int64_t V) {
  uint8_t Buf[16];
  unsigned Len = encodeSLEB128(V, Buf);
  O.append(reinterpret_cast<const char *>(Buf), Len);
}

static void emitInitStr(std::string &O, uint32_t PoolOff, uint16_t Slot) {
  O.push_back(0x47);
  appendULEB(O, PoolOff);
  appendU16(O, Slot);
}

static void emitLdStackRec(std::string &O, unsigned D, uint16_t Slot) {
  O.push_back(0x4C);
  O.push_back(static_cast<char>(D << 4));
  appendU16(O, Slot);
}

static void emitLdRaw64(std::string &O, unsigned D, unsigned Base, int64_t Disp) {
  unsigned Lo = static_cast<unsigned>(Disp) & 0xF;
  O.push_back(0x60);
  O.push_back(static_cast<char>((D << 4) | (Base & 0xF)));
  O.push_back(static_cast<char>((11 << 4) | Lo));
  appendSLEB(O, Disp >> 4);
}

static void emitAddi(std::string &O, unsigned D, unsigned L, int64_t Imm) {
  unsigned Lo = static_cast<unsigned>(Imm) & 0xF;
  O.push_back(0x38);
  O.push_back(static_cast<char>(D & 0xF));
  O.push_back(static_cast<char>((L << 4) | Lo));
  appendSLEB(O, Imm >> 4);
}

static void emitMovRR(std::string &O, unsigned D, unsigned S) {
  O.push_back(0x15);
  O.push_back(static_cast<char>((D << 4) | (S & 0xF)));
}

static void emitMovRI(std::string &O, unsigned D, int64_t Imm) {
  unsigned Lo = static_cast<unsigned>(Imm) & 0xF;
  O.push_back(0x17);
  O.push_back(static_cast<char>((D << 4) | Lo));
  appendSLEB(O, Imm >> 4);
}

static void emitLeaImage(std::string &O, unsigned D, unsigned Scratch) {
  O.push_back(0x7C);
  O.push_back(static_cast<char>((D << 4) | (Scratch & 0xF)));
  O.push_back(0);
}

static void emitCall(std::string &O, unsigned Idx, SmallVectorImpl<uint32_t> &Ends) {
  O.push_back(0x44);
  O.push_back(0x31);
  appendULEB(O, Idx);
  Ends.push_back(static_cast<uint32_t>(O.size()));
}

/// Bytes of blob B live at storage+16. The typed-slot address is not live
/// across memcpy. Reloc blob R is applied the same way, then the already
/// selected function-pointer stores run.
static std::string imagePrologue(uint32_t BlobOff, StringRef Blob, uint32_t RelocOff,
                                 StringRef Relocs, unsigned MemcpyIdx,
                                 unsigned ApplyIdx, uint32_t TupleId,
                                 SmallVectorImpl<uint32_t> &CallEnds,
                                 SmallVectorImpl<uint32_t> &Slots) {
  std::string O;
  uint16_t Slot = 0;
  if (!Blob.empty()) {
    emitInitStr(O, BlobOff, Slot);
    emitLdStackRec(O, 4, Slot);
    emitLdRaw64(O, 2, 4, 0);
    emitAddi(O, 2, 2, 16);
    emitLeaImage(O, 1, 5);
    emitMovRI(O, 3, static_cast<int64_t>(Blob.size()));
    emitCall(O, MemcpyIdx, CallEnds);
    Slots.push_back(TupleId);
    ++Slot;
  }
  if (!Relocs.empty()) {
    if (Relocs.size() % 4)
      report_fatal_error("CBC image reloc blob is not a multiple of 4");
    emitInitStr(O, RelocOff, Slot);
    emitLdStackRec(O, 5, Slot);
    emitLdRaw64(O, 2, 5, 0);
    emitAddi(O, 2, 2, 16);
    emitLeaImage(O, 1, 6);
    emitMovRI(O, 3, static_cast<int64_t>(Relocs.size() / 4));
    emitMovRR(O, 4, 1);
    emitCall(O, ApplyIdx, CallEnds);
    Slots.push_back(TupleId);
  }
  return O;
}

static std::string codeBlock(const CBCCompiledMethod &M) {
  std::string Live;
  for (uint32_t Pos : M.StatePoints) {
    appendULEB(Live, Pos);
    appendU16(Live, 0);
    appendULEB(Live, 0);
    appendULEB(Live, 0);
  }
  std::string Ex;
  for (const CBCCompiledMethod::ExRegion &R : M.Regions) {
    appendULEB(Ex, R.Start);
    appendULEB(Ex, R.End);
    appendULEB(Ex, R.Target);
  }
  std::string O;
  appendULEB(O, M.UntypedSlots);
  appendULEB(O, M.TypedSlots.size());
  for (uint32_t Term : M.TypedSlots)
    appendULEB(O, Term);
  appendULEB(O, 0); // ohm slots
  O.push_back(M.IMask);
  O.push_back(M.FMask);
  appendULEB(O, M.MaxCalleeStackArgs);
  O.push_back(M.StatePoints.empty() ? 0 : 1);
  appendULEB(O, M.UntypedMemSize);
  O.push_back(M.UsesAlloca);
  appendULEB(O, M.Code.size());
  appendULEB(O, 0); // literals
  O.append(reinterpret_cast<const char *>(M.Code.data()), M.Code.size());
  appendULEB(O, Ex.size());
  O += Ex;
  appendULEB(O, Live.size());
  O += Live;
  appendULEB(O, 0); // stack pointers
  return O;
}

std::string llvm::buildCBCFile(ArrayRef<CBCCompiledMethod> Methods,
                                 ArrayRef<CBCNativeCallee> Natives,
                                 CBCImageInfo Image, StringRef AotDeps) {
  const StringRef EntryName = "$cbc.ex:Entry";
  std::string Pool;
  auto add = [&](StringRef Bytes) {
    uint32_t Off = Pool.size();
    Pool.append(Bytes.begin(), Bytes.end());
    return Off;
  };

  std::vector<CBCNativeCallee> Callees(Natives.begin(), Natives.end());
  auto findOrAdd = [&](StringRef Name, bool Native, StringRef Params,
                       bool RetFloat) -> unsigned {
    for (unsigned I = 0, E = Callees.size(); I < E; ++I) {
      const CBCNativeCallee &C = Callees[I];
      if (C.Name == Name && C.Native == Native && C.Params == Params &&
          C.RetFloat == RetFloat)
        return I;
    }
    Callees.push_back({Name.str(), Native, Params.str(), RetFloat});
    return Callees.size() - 1;
  };
  bool NeedBlob = false, NeedRelocs = false;
  for (const CBCCompiledMethod &M : Methods) {
    if (!M.NeedsImagePrologue)
      continue;
    if (!Image.SizeBytes)
      report_fatal_error("CBC image prologue without a data image");
    NeedBlob |= !Image.Blob.empty();
    NeedRelocs |= !Image.Relocs.empty();
  }
  unsigned MemcpyIdx = 0, ApplyIdx = 0;
  if (NeedBlob)
    MemcpyIdx = findOrAdd("memcpy", true, "iii", false);
  if (NeedRelocs)
    ApplyIdx = findOrAdd("__cbc_apply_image_relocs", false, "iiii", false);

  uint32_t SEntry = add(cbcString(EntryName));
  uint32_t SAotDeps = UINT32_MAX;
  if (!AotDeps.empty())
    SAotDeps = add(cbcString(AotDeps));
  SmallVector<uint32_t, 8> NameOffs;
  for (const CBCCompiledMethod &M : Methods)
    NameOffs.push_back(add(cbcString(M.Name)));

  // Term 20: FUNCTIONAL(0 params, I64). Term 21: REF entry.
  uint32_t TSig = add(std::string("\x0C\x00\x0C", 3));
  std::string Ref = std::string("\x01", 1) + ulebStr(SEntry);
  uint32_t TRef = add(Ref);
  SmallVector<uint32_t, 8> TermOffs;
  TermOffs.push_back(TSig);
  TermOffs.push_back(TRef);

  bool AnyNative = false;
  for (const CBCNativeCallee &C : Callees)
    AnyNative |= C.Native;
  uint32_t NativeTermId = 0;
  if (AnyNative) {
    uint32_t SNative = add(cbcString("cbc.native"));
    TermOffs.push_back(add(std::string("\x02", 1) + ulebStr(SNative)));
    NativeTermId = 20 + TermOffs.size() - 1;
  }

  uint32_t DataRefId = 0, FstId = 0, SImageName = 0, SData = 0, FieldDefOff = 0;
  if (Image.SizeBytes) {
    uint32_t NU64 = std::max<uint32_t>(1, (Image.SizeBytes + 7) / 8);
    SData = add(cbcString("$cbc.ex:Data"));
    SImageName = add(cbcString("image"));
    TermOffs.push_back(add(std::string("\x01", 1) + ulebStr(SData)));
    DataRefId = 20 + TermOffs.size() - 1;
    std::string Va;
    Va.push_back(0x04);
    appendULEB(Va, NU64);
    appendULEB(Va, 13); // U64
    TermOffs.push_back(add(Va));
    uint32_t VaId = 20 + TermOffs.size() - 1;
    std::string Fst;
    Fst.push_back(0x14);
    appendULEB(Fst, VaId);
    TermOffs.push_back(add(Fst));
    FstId = 20 + TermOffs.size() - 1;
  }

  auto sigTerm = [&](const CBCNativeCallee &C) -> uint32_t {
    if (C.Params.empty() && !C.RetFloat)
      return 20;
    std::string Sig;
    Sig.push_back(0x0C);
    Sig.push_back(static_cast<char>(C.Params.size()));
    for (char K : C.Params)
      appendULEB(Sig, K == 'f' ? 19 : 12);
    appendULEB(Sig, C.RetFloat ? 19 : 12);
    TermOffs.push_back(add(Sig));
    return 20 + TermOffs.size() - 1;
  };

  SmallVector<uint32_t, 8> MRefs, SigIds, NameStr;
  for (const CBCNativeCallee &C : Callees) {
    NameStr.push_back(add(cbcString(C.Name)));
    uint32_t Sid = sigTerm(C);
    // Fix: sigTerm returns 19+size after push, which is 20+index. Good.
    SigIds.push_back(Sid);
  }
  for (size_t I = 0; I < Callees.size(); ++I) {
    std::string RefBytes;
    appendU32(RefBytes, NameStr[I]);
    if (Callees[I].Native) {
      RefBytes.push_back(0x20); // AOT
      appendULEB(RefBytes, NativeTermId);
    } else {
      RefBytes.push_back(0); // CBC method on the entry type
      appendULEB(RefBytes, 21);
    }
    appendULEB(RefBytes, SigIds[I]);
    MRefs.push_back(add(RefBytes));
  }

  uint32_t TupleId = 0, BlobOff = 0, RelocOff = 0;
  if (NeedBlob || NeedRelocs) {
    std::string Tup;
    Tup.push_back(0x12); // TUPLE
    appendULEB(Tup, 2);
    appendULEB(Tup, 13); // U64
    appendULEB(Tup, 13);
    TermOffs.push_back(add(Tup));
    TupleId = 20 + TermOffs.size() - 1;
  }
  // Blobs go in before method code so their pool offsets do not depend on it.
  if (NeedBlob)
    BlobOff = add(cbcString(Image.Blob));
  if (NeedRelocs)
    RelocOff = add(cbcString(Image.Relocs));

  SmallVector<uint32_t, 8> Codes, MDefs, MethodSigs;
  for (const CBCCompiledMethod &M : Methods) {
    CBCCompiledMethod Patched = M;
    if (M.NeedsImagePrologue && (NeedBlob || NeedRelocs)) {
      SmallVector<uint32_t, 4> CallEnds;
      SmallVector<uint32_t, 2> Slots;
      std::string Pro =
          imagePrologue(BlobOff, NeedBlob ? StringRef(Image.Blob) : StringRef(),
                        RelocOff, NeedRelocs ? StringRef(Image.Relocs) : StringRef(),
                        MemcpyIdx, ApplyIdx, TupleId, CallEnds, Slots);
      if (NeedBlob && Pro.find(static_cast<char>(0x47)) == std::string::npos)
        report_fatal_error("CBC image prologue is missing initstr");
      for (CBCCompiledMethod::ExRegion &R : Patched.Regions) {
        R.Start += Pro.size();
        R.End += Pro.size();
        R.Target += Pro.size();
      }
      std::vector<uint32_t> Points;
      Points.reserve(CallEnds.size() + Patched.StatePoints.size());
      Points.insert(Points.end(), CallEnds.begin(), CallEnds.end());
      for (uint32_t SP : Patched.StatePoints)
        Points.push_back(SP + static_cast<uint32_t>(Pro.size()));
      Patched.StatePoints = std::move(Points);
      Patched.Code.insert(Patched.Code.begin(),
                          reinterpret_cast<const uint8_t *>(Pro.data()),
                          reinterpret_cast<const uint8_t *>(Pro.data()) + Pro.size());
      Patched.TypedSlots.assign(Slots.begin(), Slots.end());
    }
    Codes.push_back(add(codeBlock(Patched)));
    CBCNativeCallee Sig;
    Sig.Params = M.Params;
    Sig.RetFloat = M.RetFloat;
    MethodSigs.push_back(sigTerm(Sig));
  }
  for (size_t I = 0; I < Methods.size(); ++I) {
    std::string Def;
    appendU32(Def, NameOffs[I]);
    appendU32(Def, SEntry);
    Def.push_back(0); // region
    appendULEB(Def, MethodSigs[I]);
    appendU16(Def, 0x0005); // PUBLIC|STATIC
    Def.push_back(0x01);
    appendULEB(Def, Codes[I]);
    Def.push_back(0);
    MDefs.push_back(add(Def));
  }

  std::string Type;
  appendU32(Type, SEntry);
  Type.push_back(0);
  appendU16(Type, 0x0001); // PUBLIC class
  appendULEB(Type, 0);     // no super
  Type += hashTable(MDefs);
  appendULEB(Type, 0); // virtual methods
  Type += hashTable({});
  appendULEB(Type, 0); // instance fields
  Type.push_back(0);
  uint32_t TDef = add(Type);
  SmallVector<uint32_t, 4> TypeDefs;
  TypeDefs.push_back(TDef);

  SmallVector<uint32_t, 2> FRefs;
  if (Image.SizeBytes) {
    std::string FDef;
    appendU32(FDef, SImageName);
    FDef.push_back(0);
    appendULEB(FDef, FstId);
    FDef.push_back(0x05); // PUBLIC|STATIC
    FDef.push_back(0);
    FieldDefOff = add(FDef);

    std::string FRef;
    FRef.push_back(0); // SINGLE
    appendU32(FRef, SImageName);
    appendULEB(FRef, DataRefId);
    appendULEB(FRef, FstId);
    FRefs.push_back(add(FRef));

    std::string DType;
    appendU32(DType, SData);
    DType.push_back(0);
    appendU16(DType, 0x0001);
    appendULEB(DType, 0);
    DType += hashTable({});
    appendULEB(DType, 0);
    SmallVector<uint32_t, 1> OneField{FieldDefOff};
    DType += hashTable(OneField);
    appendULEB(DType, 0);
    DType.push_back(0);
    TypeDefs.push_back(add(DType));
  }

  SmallVector<uint32_t, 8> AotEntryOffs;
  for (size_t I = 0; I < Callees.size(); ++I) {
    if (!Callees[I].Native)
      continue;
    std::string Ent;
    appendU32(Ent, I);
    appendU32(Ent, NameStr[I]);
    AotEntryOffs.push_back(add(Ent));
  }

  const uint32_t Base = 57;
  uint32_t TypeIndexAt = Base + Pool.size();
  std::string TypeIndex = hashTable(TypeDefs);
  std::string Direct = hashTable(AotEntryOffs);
  std::string Empty = hashTable({});
  std::string Aot = Direct + Empty + Empty + Empty + Empty;
  uint32_t AotOffs[5];
  AotOffs[0] = TypeIndexAt + TypeIndex.size();
  AotOffs[1] = AotOffs[0] + Direct.size();
  AotOffs[2] = AotOffs[1] + Empty.size();
  AotOffs[3] = AotOffs[2] + Empty.size();
  AotOffs[4] = AotOffs[3] + Empty.size();
  uint32_t AfterAot = AotOffs[4] + Empty.size();
  std::string MRefArr, FRefArr, TermArr;
  for (uint32_t O : MRefs)
    appendU32(MRefArr, O);
  for (uint32_t O : FRefs)
    appendU32(FRefArr, O);
  for (uint32_t O : TermOffs)
    appendU32(TermArr, O);
  (void)TSig;
  uint32_t MRefAt = AfterAot;
  uint32_t FRefAt = MRefAt + MRefArr.size();
  uint32_t TermAt = FRefAt + FRefArr.size();
  uint32_t ExtAt = TermAt + TermArr.size();
  std::string Ext = ulebStr(0);
  uint32_t RegionAt = ExtAt + Ext.size();
  while (RegionAt % 4) {
    Ext.push_back(0);
    ++RegionAt;
  }
  std::string Region;
  appendU16(Region, 0);
  appendU32(Region, 0);
  appendULEB(Region, Callees.size());
  appendU32(Region, MRefAt);
  appendULEB(Region, FRefs.size());
  appendU32(Region, FRefAt);
  appendULEB(Region, TermOffs.size());
  appendU32(Region, TermAt);

  std::string Header(57, '\0');
  auto putU32 = [&](unsigned At, uint32_t V) {
    for (int I = 0; I < 4; ++I)
      Header[At + I] = static_cast<char>((V >> (8 * I)) & 0xFF);
  };
  Header[0] = 'C';
  Header[1] = 'B';
  Header[2] = 'C';
  Header[3] = 1;
  putU32(4, TypeIndexAt);
  putU32(8, 57);
  for (int I = 0; I < 5; ++I)
    putU32(12 + 4 * I, AotOffs[I]);
  putU32(32, ExtAt);
  putU32(36, RegionAt);
  auto putS32 = [&](unsigned At, int32_t V) { putU32(At, static_cast<uint32_t>(V)); };
  putS32(40, static_cast<int32_t>(SEntry));
  putS32(44, -1);
  putS32(48, SAotDeps == UINT32_MAX ? -1 : static_cast<int32_t>(SAotDeps));
  putS32(52, -1);

  return Header + Pool + TypeIndex + Aot + MRefArr + FRefArr + TermArr + Ext + Region;
}
