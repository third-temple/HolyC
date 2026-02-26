I64 Main()
{
  I64 a=0;
  I64 *p;
  p=&a;
  p=p+1;
  p=p-1;
  return p-p;
}
