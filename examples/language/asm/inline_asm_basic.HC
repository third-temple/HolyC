I64 Main()
{
  asm("mov rax, rax", "={rax}", "{rax}");
  asm {
    MOV RAX,RAX
    NOP
  }
  return 0;
}
