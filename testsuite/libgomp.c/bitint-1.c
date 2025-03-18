/* PR middle-end/113409 */
/* { dg-do run { target bitint } } */

extern void abort (void);

#if __BITINT_MAXWIDTH__ >= 1023
typedef _BitInt(931) B931;
typedef _BitInt(1023) B1023;
#else
typedef _BitInt(31) B931;
typedef _BitInt(63) B1023;
#endif

__attribute__((noipa)) B931
bar (B931 x)
{
  return x;
}

B931
foo (B931 x)
{
  B931 r = 0;
  B1023 l = 56wb;
  #pragma omp parallel for reduction(+: r) linear(l : 3wb)
  for (B931 i = 0; i < x; ++i)
    {
      r += bar (i);
      l += 3wb;
    }
  if (l != (B1023) 56wb + x * 3wb)
    abort ();
  return r;
}

B931
baz (B931 a, B931 b, B931 c, B931 d, B931 e, B931 f)
{
  B931 r = 0;
  #pragma omp parallel for collapse (2wb) reduction(+: r)
  for (B931 i = a; i < b; i += c)
    for (B931 j = d; j > e; j += f)
{
      r += (j - d) / f;
__builtin_printf ("%d\n", (int) r);
}
  return r;
}

int
main ()
{
  if (foo (16wb) != (B931) 15wb * 16wb / 2
      || foo (256wb) != (B931) 255wb * 256wb / 2)
    abort ();
#if __BITINT_MAXWIDTH__ >= 1023
  if (baz (5019676379303764570412381742937286053482001129028025397398691108125646744814606405323608429353439158482254231750681261083217232780938592007150824765654203477280876662295642053702075485153212701225737143207062700602509893062044376997132415613866154761073993220684129908568716699977wb,
           5019676379303764570412381742937286053482001129028025397398691108125646744814606405323608429353439158482254231750681261083217232780938592007150824765654203477280876662295642053702075485153212701225737143207062700602509893062044376997132415613866154761074023903103954348393149593648wb,
           398472984732984732894723wb,
           5145599438319070078334010989312672300490893251953772234670751860213136881221517063143096309285807356778798661066289865489661268190588670564647904159660341525674865064477335008915374460378741763629714814990575971883514175167056160470289039998140910732382754821232566561860399556131wb,
           5145599438319070078334010989312672300490893251953772234670751860213136881221517063143096309285807356778798661066289865489661268190588670564647904159660341525674865064477335008915374460378741763629714814990575971883514175167056160470289039995725068563774878643356388697468035164019wb,
           -89475635874365784365784365347865347856wb) != (B931) 26wb * 27wb / 2wb * 77wb)
    abort ();
#endif
}
