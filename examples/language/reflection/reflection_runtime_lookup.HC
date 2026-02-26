class Widget {
  I64 id;
  U8 *name;
};

I64 hc_reflection_field_count();

I64 Main()
{
  return hc_reflection_field_count();
}
