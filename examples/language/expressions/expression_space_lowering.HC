class Pair
{
  I64 a;
};

I64 Main()
{
  Pair p;
  I64* ptr;
  I64 i = 0;
  return (i++, p.a, ptr[0], (I64)i);
}
