I64 Recurse(I64 n)
{
  if (n)
    return 1 + CallStkGrow(0x100, 0x1000, &Recurse, n - 1);
  return 0;
}

I64 Main()
{
  return Recurse(5);
}
