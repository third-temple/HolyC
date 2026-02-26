typedef I64 Count;
typedef I64 (*Reducer)(I64, I64);

class Vec2
{
  F64 x;
  F64 y;
};

union Slot
{
  I64 iv;
  U8 *ptr;
};

I64 Apply(Reducer fn, I64 a, I64 b);

Count Main()
{
  Count c;
  return c;
}
