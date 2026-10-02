#include "llvm/MC/MCCBCObjectWriter.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"

using namespace llvm;

uint64_t CBCObjectWriter::writeObject() {
  uint64_t Start = OS.tell();
  // CBCAsmPrinter emits the complete file image into the text section.
  for (const MCSection &S : *Asm)
    Asm->writeSectionData(OS, &S);
  return OS.tell() - Start;
}

std::unique_ptr<MCObjectWriter>
llvm::createCBCObjectWriter(std::unique_ptr<MCCBCObjectTargetWriter> MOTW,
                            raw_pwrite_stream &OS) {
  return std::make_unique<CBCObjectWriter>(std::move(MOTW), OS);
}
