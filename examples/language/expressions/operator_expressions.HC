I64 Main()
{
  I64 x=10;
  x+=5;
  x-=3;
  x*=2;
  x/=4;
  x%=4;
  x<<=2;
  x>>=1;
  x|=8;
  x&=14;
  x^=3;
  x=+x;
  x=-x;
  x=~x;
  if (!0) {
    x+=1;
  }
  return x;
}
