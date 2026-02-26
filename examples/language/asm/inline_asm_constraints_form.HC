I64 Main()
{
  asm("mov rax, rax", "={rax}");
  return 0;
}
