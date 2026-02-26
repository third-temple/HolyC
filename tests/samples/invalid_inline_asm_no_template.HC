I64 Main()
{
  asm(123, "={rax}", "{rax}");
  return 0;
}
