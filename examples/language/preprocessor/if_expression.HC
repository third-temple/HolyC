#define A 2
#define B 3
#define SCALE 2

#if defined(A) && ((A + B * SCALE) == 8) && !defined(MISSING)
I64 Main()
{
  return 42;
}
#elif 1
I64 Main()
{
  return 1;
}
#else
I64 Main()
{
  return 0;
}
#endif
