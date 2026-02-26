I64 Add1(I64 x)
{
  return x + 1;
}

I64 Main()
{
  I64 (*fp)(I64) = &Add1;
  return fp(42);
}
