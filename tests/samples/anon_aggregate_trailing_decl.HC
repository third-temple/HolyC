class {
  I64 x;
  I64 y;
} g_pair;

I64 Main()
{
  g_pair.x = 4;
  g_pair.y = 2;
  return g_pair.x + g_pair.y;
}
