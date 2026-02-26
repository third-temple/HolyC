U0 StructName(U8 *d, U8 *class_name=lastclass)
{
  "%s\n", class_name;
}

class Student
{
  I64 age;
} a;

class School
{
  I64 id;
} s;

I64 i;

I64 Main()
{
  StructName(&a);
  StructName(&s);
  StructName(&i);
  return 0;
}
