public I64 Add(reg I64 a, noreg I64 b)
{
  return a + b;
}

interrupt U0 Tick()
{
  no_warn I64 ignored = 0;
  ignored = ignored + 1;
}

I64 Main()
{
  Tick;
  return Add(1, 2);
}
