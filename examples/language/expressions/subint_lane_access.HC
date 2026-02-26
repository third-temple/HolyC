I64 Main()
{
  I64 q = 0xFEDCBA9876543210;
  I64 q1;
  q1 = q.i16[2];
  q1 = q.u8[5];
  q1 = q.i32[1].u8[2];
  q1 = q.i32[0].i8[1];
  return q1;
}
