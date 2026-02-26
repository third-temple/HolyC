I64 a = 5;
I64 b = a + 3;
I64 *p = &a;

I64 Main()
{
  if (p == &a)
    return b;
  return 0;
}
