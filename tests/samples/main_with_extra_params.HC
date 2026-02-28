I64 Main(I64 argc, U8 **argv, I64 extra)
{
  if (argc > 0 && argv != 0 && extra == 0)
    return 0;
  return 1;
}
