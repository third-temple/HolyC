I64 Main()
{
  I64 x = 1;
  asm("", "r", x += 4);
  return x;
}
