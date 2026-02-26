I64 Src()
{
  return 42;
}

I64 Main()
{
  F64 a = Src()(F64);
  I64 b = a(I64);
  return b;
}
