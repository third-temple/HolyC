class Student
{
  I64 age;
} a,b;

union Slot
{
  I64 iv;
  U8 *ptr;
} s;

I64 Main()
{
  a.age=7;
  b.age=3;
  s.iv=a.age+b.age;
  return s.iv;
}
