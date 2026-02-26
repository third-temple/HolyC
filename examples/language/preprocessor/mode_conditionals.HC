#ifjit
I64 ModeValue()
{
  return 10;
}
#else
I64 ModeValue()
{
  return 20;
}
#endif

I64 Main()
{
  return ModeValue();
}
