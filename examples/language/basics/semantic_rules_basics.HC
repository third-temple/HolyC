U0 Greet(U8 *name="world")
{
  "%s\n",name;
}

I64 Add(I64 a=1,I64 b=2)
{
  return a+b;
}

U0 Main()
{
  I64 x=10;
  if (5<x<20)
    "range\n";
  Greet;
  Add(,3);
}

Main;
