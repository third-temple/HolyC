I64 g_count = 40 + 2;
U8 *g_msg = "ok";

I64 Main()
{
  return g_count + (g_msg != 0);
}
