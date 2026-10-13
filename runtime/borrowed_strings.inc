/* Engine query results are borrowed, unlike callback argument copies. Reuse
 * equal strings instead of allocating another permanent block every frame.
 * Content identity also avoids aliasing unrelated native scratch buffers. */
typedef struct BorrowedText {uint32_t guest;struct BorrowedText *next;} BorrowedText;
static BorrowedText *borrowed_text[1024];
static uint32_t borrowed_string(const char *text)
{
    if(!text)return 0;uint32_t hash=2166136261u;
    for(const unsigned char *p=(const unsigned char*)text;*p;p++)hash=(hash^*p)*16777619u;
    hash&=1023;for(BorrowedText *p=borrowed_text[hash];p;p=p->next)if(!strcmp(gs(p->guest),text))return p->guest;
    BorrowedText *p=calloc(1,sizeof *p);if(!p)abort();p->guest=copy_string(text);p->next=borrowed_text[hash];borrowed_text[hash]=p;return p->guest;
}
