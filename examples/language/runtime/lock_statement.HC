I64 Main()
{
  I64 x = 0;
  lock x += 1;
  lock {
    x += 2;
    x += 1;
  }
  return x;
}
