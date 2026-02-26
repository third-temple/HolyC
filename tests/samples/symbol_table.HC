extern I64 g_ext;
I64 g_value;

class C
{
  I64 f;
};

I64 Helper(I64 x)
{
  return x+g_value;
}

I64 Main()
{
  I64 g_value=5;
  g_value=g_value+1;
  return Helper(g_value)+g_ext;
}
