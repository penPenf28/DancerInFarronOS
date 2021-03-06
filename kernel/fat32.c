#include "../libs/param.h"
#include "../libs/types.h"
#include "../libs/riscv.h"
#include "../libs/spinlock.h"
#include "../libs/sleeplock.h"
#include "../libs/buf.h"
#include "../libs/proc.h"
#include "../libs/stat.h"
#include "../libs/fat32.h"
#include "../libs/string.h"
#include "../libs/printf.h"

/* fields that start with "_" are something we don't use */

typedef struct short_name_entry {
    char        name[CHAR_SHORT_NAME];
    uint8       attr;
    uint8       _nt_res;
    uint8       _crt_time_tenth;
    uint16      _crt_time;
    uint16      _crt_date;
    uint16      _lst_acce_date;
    uint16      fst_clus_hi;
    uint16      _lst_wrt_time;
    uint16      _lst_wrt_date;
    uint16      fst_clus_lo;
    uint32      file_size;
} __attribute__((packed, aligned(4))) short_name_entry_t;

typedef struct long_name_entry {
    uint8       order;
    wchar       name1[5];
    uint8       attr;
    uint8       _type;
    uint8       checksum;
    wchar       name2[6];
    uint16      _fst_clus_lo;
    wchar       name3[2];
} __attribute__((packed, aligned(4))) long_name_entry_t;

union dentry {
    short_name_entry_t  sne;
    long_name_entry_t   lne;
};

static struct {
    uint32  first_data_sec;         //第一个数据扇区号
    uint32  data_sec_cnt;           //数据扇区的数量
    uint32  data_clus_cnt;          //数据簇的数量
    uint32  byts_per_clus;          //每个簇的字节数

    struct {
        uint16  byts_per_sec;       //每个扇区字节数
        uint8   sec_per_clus;       //每个簇的扇区数
        uint16  rsvd_sec_cnt;       //保留区的扇区数
        uint8   fat_cnt;            //fat表的数量
        uint32  hidd_sec;           //每个fat表的扇区数
        uint32  tot_sec;            //总扇区数
        uint32  fat_sz;             //fat表的大小
        uint32  root_clus;          //根目录的簇号
    } bpb;//0号扇区，存放文件系统的参数

} fat;

static struct entry_cache {
    struct spinlock lock;
    struct dirent entries[ENTRY_CACHE_NUM];
} ecache;

static struct dirent root;

/**
 * Read the Boot Parameter Block.
 * @return  0       if success
 *          -1      if fail
 */
int fat32_init()
{
    #ifdef DEBUG
    printf("[fat32_init] enter!\n");
    #endif
    //去高速缓存块中获取dev=0的0号簇（即bpb，存放着文件系统的参数）
    struct buf *b = bread(0, 0);


    if (strncmp((char const*)(b->data + 82), "FAT32", 5))
        panic("not FAT32 volume");

    // fat.bpb.byts_per_sec = *(uint16 *)(b->data + 11);
    // 给bpb（Bios Parameter Block）赋值
    memmove(&fat.bpb.byts_per_sec, b->data + 11, 2);            // avoid misaligned load on k210
    fat.bpb.sec_per_clus = *(b->data + 13);
    fat.bpb.rsvd_sec_cnt = *(uint16 *)(b->data + 14);
    fat.bpb.fat_cnt = *(b->data + 16);
    fat.bpb.hidd_sec = *(uint32 *)(b->data + 28);
    fat.bpb.tot_sec = *(uint32 *)(b->data + 32);
    fat.bpb.fat_sz = *(uint32 *)(b->data + 36);
    fat.bpb.root_clus = *(uint32 *)(b->data + 44);

    //计算出文件系统的第一个数据扇区的扇区号=（保留扇区+fat表数量*fat表大小）
    fat.first_data_sec = fat.bpb.rsvd_sec_cnt + fat.bpb.fat_cnt * fat.bpb.fat_sz;
    //数据扇区的数量=总的扇区数-第一个数据扇区号
    fat.data_sec_cnt = fat.bpb.tot_sec - fat.first_data_sec;
    //数据簇的数量=数据扇区数量/每个簇的大小
    fat.data_clus_cnt = fat.data_sec_cnt / fat.bpb.sec_per_clus;
    //每个簇的字节数=每个簇的扇区数量*每个扇区的大小
    fat.byts_per_clus = fat.bpb.sec_per_clus * fat.bpb.byts_per_sec;
    //释放掉高速缓存块b的一个引用
    brelse(b);

    #ifdef DEBUG
    printf("[FAT32 init]byts_per_sec: %d\n", fat.bpb.byts_per_sec);
    printf("[FAT32 init]root_clus: %d\n", fat.bpb.root_clus);
    printf("[FAT32 init]sec_per_clus: %d\n", fat.bpb.sec_per_clus);
    printf("[FAT32 init]fat_cnt: %d\n", fat.bpb.fat_cnt);
    printf("[FAT32 init]fat_sz: %d\n", fat.bpb.fat_sz);
    printf("[FAT32 init]first_data_sec: %d\n", fat.first_data_sec);
    #endif

    // make sure that byts_per_sec has the same value with BSIZE 
    if (BSIZE != fat.bpb.byts_per_sec) 
        panic("byts_per_sec != BSIZE");

    //为ecache添加一个互斥锁
    initlock(&ecache.lock, "ecache");

    //把根目录清零
    memset(&root, 0, sizeof(root));
    //为根目录添加一个entry锁
    initsleeplock(&root.lock, "entry");


    root.attribute = (ATTR_DIRECTORY | ATTR_SYSTEM);
    //为根目录分配第一个簇号，并指定当前指向的簇号
    root.first_clus = root.cur_clus = fat.bpb.root_clus;
    root.valid = 1;
    root.prev = &root;
    root.next = &root;
    //初始化ecache数组（文件集合）
    for(struct dirent *de = ecache.entries; de < ecache.entries + ENTRY_CACHE_NUM; de++) {
        de->dev = 0;
        de->valid = 0;
        de->ref = 0;
        de->dirty = 0;
        de->parent = 0;
        de->next = root.next;
        de->prev = &root;
        initsleeplock(&de->lock, "entry");
        root.next->prev = de;
        root.next = de;
    }
    return 0;
}

/**
 * @param   cluster   cluster number starts from 2, which means no 0 and 1
 */
static inline uint32 first_sec_of_clus(uint32 cluster)
{
    return ((cluster - 2) * fat.bpb.sec_per_clus) + fat.first_data_sec;
}

/**
 * For the given number of a data cluster, return the number of the sector in a FAT table.
 * @param   cluster     number of a data cluster
 * @param   fat_num     number of FAT table from 1, shouldn't be larger than bpb::fat_cnt
 */
// 给定一个数据簇号，和第几个fat表，返回在这个fat表中的哪一个扇区
// 直白的说就是寻找fat表中对应的扇区
static inline uint32 fat_sec_of_clus(uint32 cluster, uint8 fat_num)
{
    return fat.bpb.rsvd_sec_cnt + (cluster << 2) / fat.bpb.byts_per_sec + fat.bpb.fat_sz * (fat_num - 1);
}

/**
 * For the given number of a data cluster, return the offest in the corresponding sector in a FAT table.
 * @param   cluster   number of a data cluster
 */
// 给定簇号获得他在fat表中的偏移
static inline uint32 fat_offset_of_clus(uint32 cluster)
{
    return (cluster << 2) % fat.bpb.byts_per_sec;
}

/**
 * Read the FAT table content corresponded to the given cluster number.
 * @param   cluster     the number of cluster which you want to read its content in FAT table
 */
// 基于给定的簇号读取相应的fat表扇区，并返回下一个簇号
static uint32 read_fat(uint32 cluster)
{
    if (cluster >= FAT32_EOC) {
        return cluster;
    }
    if (cluster > fat.data_clus_cnt + 1) {     // because cluster number starts at 2, not 0
        return 0;
    }
    //找到簇号在 fat表1 对应的扇区
    uint32 fat_sec = fat_sec_of_clus(cluster, 1);
    //b映射一个fatsec缓存到高速缓存块中
    struct buf *b = bread(0, fat_sec);
    //获取下一个簇号
    uint32 next_clus = *(uint32 *)(b->data + fat_offset_of_clus(cluster));
    //释放b的一个引用
    brelse(b);
    return next_clus;
}

/**
 * Write the FAT region content corresponded to the given cluster number.
 * @param   cluster     the number of cluster to write its content in FAT table
 * @param   content     the content which should be the next cluster number of FAT end of chain flag
 */
static int write_fat(uint32 cluster, uint32 content)
{
    if (cluster > fat.data_clus_cnt + 1) {
        return -1;
    }
    //获取簇在fat中的扇区号
    uint32 fat_sec = fat_sec_of_clus(cluster, 1);
    struct buf *b = bread(0, fat_sec);
    uint off = fat_offset_of_clus(cluster);
    //下面两行代码是将content中的内容写到b的扇区号里
    *(uint32 *)(b->data + off) = content;
    bwrite(b);
    
    brelse(b);
    return 0;
}

//清零簇中的数据，并写入到磁盘中
static void zero_clus(uint32 cluster)
{
    uint32 sec = first_sec_of_clus(cluster);
    struct buf *b;
    for (int i = 0; i < fat.bpb.sec_per_clus; i++) {
        b = bread(0, sec++);
        memset(b->data, 0, BSIZE);
        bwrite(b);
        brelse(b);
    }
}

//从dev中分配一个簇
static uint32 alloc_clus(uint8 dev)
{
    // should we keep a free cluster list? instead of searching fat every time.
    struct buf *b;
    uint32 sec = fat.bpb.rsvd_sec_cnt;

    //每个扇区的表项数
    uint32 const ent_per_sec = fat.bpb.byts_per_sec / sizeof(uint32);

    //遍历fat表
    for (uint32 i = 0; i < fat.bpb.fat_sz; i++, sec++) {
        //读取dev中的sec保存到一个buffer中并返回到b
        b = bread(dev, sec);
        //遍历buffer中所有表项
        for (uint32 j = 0; j < ent_per_sec; j++) {
            //如果未分配，初始化该簇并返回
            if (((uint32 *)(b->data))[j] == 0) {
                ((uint32 *)(b->data))[j] = FAT32_EOC + 7;
                bwrite(b);
                brelse(b);
                uint32 clus = i * ent_per_sec + j;
                zero_clus(clus);
                return clus;
            }
        }
        brelse(b);
    }
    panic("no clusters");
}

//清空簇
static void free_clus(uint32 cluster)
{
    write_fat(cluster, 0);
}

//对簇进行读写,从off开始的n个字节
static uint rw_clus(uint32 cluster, int write, int user, uint64 data, uint off, uint n)
{
    if (off + n > fat.byts_per_clus)
        panic("offset out of range");
    uint tot, m;
    struct buf *bp;
    //利用簇和偏移获得对应的扇区号
    uint sec = first_sec_of_clus(cluster) + off / fat.bpb.byts_per_sec;
    off = off % fat.bpb.byts_per_sec;

    int bad = 0;
    for (tot = 0; tot < n; tot += m, off += m, data += m, sec++) {
        // 读取sec内的内容
        bp = bread(0, sec);
        // m为剩余大小
        m = BSIZE - off % BSIZE;
        
        if (n - tot < m) {
            m = n - tot;
        }
        if (write) {
            // 写入操作
            // either_copyin根据第二个参数判断从用户地址复制还是从内核地址复制到bp->data
            if ((bad = either_copyin(bp->data + (off % BSIZE), user, data, m)) != -1) {
                bwrite(bp);
            }
        } else {
            // 复制到第二个参数，源是第三个参数
            bad = either_copyout(user, data, bp->data + (off % BSIZE), m);
        }
        // 释放bp的一个引用
        brelse(bp);
        if (bad == -1) {
            break;
        }
    }
    return tot;
}

/**
 * for the given entry, relocate the cur_clus field based on the off
 * @param   entry       modify its cur_clus field ，修改该entry指向的cur_clus
 * @param   off         the offset from the beginning of the relative file，从相关文件开始的偏移
 * @param   alloc       whether alloc new cluster when meeting end of FAT chains，当遇到FAT链尾是否分配一个新簇
 * @return              the offset from the new cur_clus 
 */
// 给定entry，根据偏移重新分配簇，返回新的当前簇
static int reloc_clus(struct dirent *entry, uint off, int alloc)
{
    // 计算出偏移处簇的数量
    int clus_num = off / fat.byts_per_clus;
    // 如果大于，则一直分配到偏移
    while (clus_num > entry->clus_cnt) {
        // 根据当前簇号返回下一个簇号clus
        int clus = read_fat(entry->cur_clus);
        if (clus >= FAT32_EOC) {
            if (alloc) {
                clus = alloc_clus(entry->dev);
                //分配完新簇后写入到当前cur_clus
                write_fat(entry->cur_clus, clus);
            } else {
                entry->cur_clus = entry->first_clus;
                entry->clus_cnt = 0;
                return -1;
            }
        }
        // 修改指向
        entry->cur_clus = clus;
        entry->clus_cnt++;
    }
    // 如果小于
    if (clus_num < entry->clus_cnt) {
        // 重新从第一个开始分配至偏移处
        entry->cur_clus = entry->first_clus;
        entry->clus_cnt = 0;
        while (entry->clus_cnt < clus_num) {
            entry->cur_clus = read_fat(entry->cur_clus);
            if (entry->cur_clus >= FAT32_EOC) {
                panic("reloc_clus");
            }
            entry->clus_cnt++;
        }
    }
    return off % fat.byts_per_clus;
}

/* like the original readi, but "reade" is odd, let alone "writee" */
// Caller must hold entry->lock.
// 向entry里读数据
// 给定entry，将off起始的n个字节读取到dst处
int eread(struct dirent *entry, int user_dst, uint64 dst, uint off, uint n)
{
    if (off > entry->file_size || off + n < off || (entry->attribute & ATTR_DIRECTORY)) {
        return 0;
    }
    // 如果大于文件大小，则一直到文件末尾
    if (off + n > entry->file_size) {
        n = entry->file_size - off;
    }

    uint tot, m;
    for (tot = 0; entry->cur_clus < FAT32_EOC && tot < n; tot += m, off += m, dst += m) {
        // 第三个参数为0，如果到末尾不重新分配新簇
        reloc_clus(entry, off, 0);
        // m为当前簇剩余的字节数
        m = fat.byts_per_clus - off % fat.byts_per_clus;

        if (n - tot < m) {
            m = n - tot;
        }
        // 更新m，从off开始的m个字节
        // 对entry->cur_clus进行读，第二个参数是write，0为读，第三个参数判断是从用户地址还是内核地址
        // 第四个参数dst读完的数据放入的data，第六个参数是偏移量，m是从偏移量开始的m个字节
        if (rw_clus(entry->cur_clus, 0, user_dst, dst, off % fat.byts_per_clus, m) != m) {
            break;
        }
    }
    return tot;
}

// Caller must hold entry->lock.
// 向entry里写数据
// 给定entry，将off起始的n个字节写到dst处
int ewrite(struct dirent *entry, int user_src, uint64 src, uint off, uint n)
{
    if (off > entry->file_size || off + n < off || (uint64)off + n > 0xffffffff
        || (entry->attribute & ATTR_READ_ONLY)) {
        return -1;
    }
    // 如果文件大小为0，则新分配一个簇
    if (entry->first_clus == 0) {   // so file_size if 0 too, which requests off == 0
        entry->cur_clus = entry->first_clus = alloc_clus(entry->dev);
        entry->clus_cnt = 0;
        entry->dirty = 1;
    }
    uint tot, m;
    for (tot = 0; tot < n; tot += m, off += m, src += m) {
        reloc_clus(entry, off, 1);
        m = fat.byts_per_clus - off % fat.byts_per_clus;
        if (n - tot < m) {
            m = n - tot;
        }
        // 从src读取数据，偏移off+m个字节的数据到entry->cur_clus
        if (rw_clus(entry->cur_clus, 1, user_src, src, off % fat.byts_per_clus, m) != m) {
            break;
        }
    }
    if(n > 0) {
        // 更新文件大小
        if(off > entry->file_size) {
            entry->file_size = off;
            entry->dirty = 1;
        }
    }
    return tot;
}

// Returns a dirent struct. If name is given, check ecache. It is difficult to cache entries
// by their whole path. But when parsing a path, we open all the directories through it, 
// which forms a linked list from the final file to the root. Thus, we use the "parent" pointer 
// to recognize whether an entry with the "name" as given is really the file we want in the right path.
// Should never get root by eget, it's easy to understand.

// 返回一个dirent结构
static struct dirent *eget(struct dirent *parent, char *name)
{
    struct dirent *ep;
    acquire(&ecache.lock);
    // 如果有name参数，检查ecache
    if (name) {
        // 双向循环链表，检查回来退出循环，从前往后找最常用的
        for (ep = root.next; ep != &root; ep = ep->next) {          // LRU 算法
            // 如果有效，且他的父节点相等，且文件名相等
            // 则该文件结构引用+1，父亲引用如果没分配也+1
            if (ep->valid == 1 && ep->parent == parent
                && strncmp(ep->filename, name, FAT32_MAX_FILENAME) == 0) {
                if (ep->ref++ == 0) {
                    ep->parent->ref++;
                }
                release(&ecache.lock);
                // edup(ep->parent);
                // 返回该文件结构dirent
                return ep;
            }
        }
    }
    // 如果不给name，直接从后向前找到最少使用的一个dirent项进行返回
    for (ep = root.prev; ep != &root; ep = ep->prev) {              // LRU 算法
        if (ep->ref == 0) {
            ep->ref = 1;
            ep->dev = parent->dev;
            ep->off = 0;
            ep->valid = 0;
            ep->dirty = 0;
            release(&ecache.lock);
            return ep;
        }
    }
    panic("eget: insufficient ecache");
    return 0;
}

// trim ' ' in the head and tail, '.' in head, and test legality
// 格式化name
char *formatname(char *name)
{
    static char illegal[] = { '\"', '*', '/', ':', '<', '>', '?', '\\', '|', 0 };
    char *p;
    // 如果开头是空格或.则跳过
    while (*name == ' ' || *name == '.') { name++; }

    for (p = name; *p; p++) {
        char c = *p;
        //如果有非法字符直接返回
        if (c < 0x20 || strchr(illegal, c)) {
            return 0;
        }
    }
    while (p-- > name) {
        //去掉name后面的空格符号
        if (*p != ' ') {
            p[1] = '\0';
            break;
        }
    }
    return name;
}

// 输入name，生成shortname
static void generate_shortname(char *shortname, char *name)
{
    static char illegal[] = { '+', ',', ';', '=', '[', ']', 0 };   // these are legal in l-n-e but not s-n-e
    int i = 0;
    char c, *p = name;
    for (int j = strlen(name) - 1; j >= 0; j--) {
        if (name[j] == '.') {
            p = name + j;
            break;
        }
    }
    while (i < CHAR_SHORT_NAME && (c = *name++)) {
        if (i == 8 && p) {
            if (p + 1 < name) { break; }            // no '.'
            else {
                name = p + 1, p = 0;
                continue;
            }
        }
        if (c == ' ') { continue; }
        if (c == '.') {
            if (name > p) {                    // last '.'
                memset(shortname + i, ' ', 8 - i);
                i = 8, p = 0;
            }
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c += 'A' - 'a';
        } else {
            if (strchr(illegal, c) != NULL) {
                c = '_';
            }
        }
        shortname[i++] = c;
    }
    while (i < CHAR_SHORT_NAME) {
        shortname[i++] = ' ';
    }
}

uint8 cal_checksum(uchar* shortname)
{
    uint8 sum = 0;
    for (int i = CHAR_SHORT_NAME; i != 0; i--) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *shortname++;
    }
    return sum;
}

/**
 * Generate an on disk format entry and write to the disk. Caller must hold dp->lock
 * @param   dp          the directory
 * @param   ep          entry to write on disk
 * @param   off         offset int the dp, should be calculated via dirlookup before calling this
 */
// 类似于imake
void emake(struct dirent *dp, struct dirent *ep, uint off)
{
    if (!(dp->attribute & ATTR_DIRECTORY))
        panic("emake: not dir");
    if (off % sizeof(union dentry))
        panic("emake: not aligned");
    
    union dentry de;
    memset(&de, 0, sizeof(de));
    if (off <= 32) {
        if (off == 0) {
            strncpy(de.sne.name, ".          ", sizeof(de.sne.name));
        } else {
            strncpy(de.sne.name, "..         ", sizeof(de.sne.name));
        }
        de.sne.attr = ATTR_DIRECTORY;
        de.sne.fst_clus_hi = (uint16)(ep->first_clus >> 16);        // first clus high 16 bits
        de.sne.fst_clus_lo = (uint16)(ep->first_clus & 0xffff);       // low 16 bits
        de.sne.file_size = 0;                                       // filesize is updated in eupdate()
        off = reloc_clus(dp, off, 1);
        rw_clus(dp->cur_clus, 1, 0, (uint64)&de, off, sizeof(de));
    } else {
        int entcnt = (strlen(ep->filename) + CHAR_LONG_NAME - 1) / CHAR_LONG_NAME;   // count of l-n-entries, rounds up
        char shortname[CHAR_SHORT_NAME + 1];
        memset(shortname, 0, sizeof(shortname));
        generate_shortname(shortname, ep->filename);
        de.lne.checksum = cal_checksum((uchar *)shortname);
        de.lne.attr = ATTR_LONG_NAME;
        for (int i = entcnt; i > 0; i--) {
            if ((de.lne.order = i) == entcnt) {
                de.lne.order |= LAST_LONG_ENTRY;
            }
            char *p = ep->filename + (i - 1) * CHAR_LONG_NAME;
            uint8 *w = (uint8 *)de.lne.name1;
            int end = 0;
            for (int j = 1; j <= CHAR_LONG_NAME; j++) {
                if (end) {
                    *w++ = 0xff;            // on k210, unaligned reading is illegal
                    *w++ = 0xff;
                } else { 
                    if ((*w++ = *p++) == 0) {
                        end = 1;
                    }
                    *w++ = 0;
                }
                switch (j) {
                    case 5:     w = (uint8 *)de.lne.name2; break;
                    case 11:    w = (uint8 *)de.lne.name3; break;
                }
            }
            uint off2 = reloc_clus(dp, off, 1);
            rw_clus(dp->cur_clus, 1, 0, (uint64)&de, off2, sizeof(de));
            off += sizeof(de);
        }
        memset(&de, 0, sizeof(de));
        strncpy(de.sne.name, shortname, sizeof(de.sne.name));
        de.sne.attr = ep->attribute;
        de.sne.fst_clus_hi = (uint16)(ep->first_clus >> 16);      // first clus high 16 bits
        de.sne.fst_clus_lo = (uint16)(ep->first_clus & 0xffff);     // low 16 bits
        de.sne.file_size = ep->file_size;                         // filesize is updated in eupdate()
        off = reloc_clus(dp, off, 1);
        rw_clus(dp->cur_clus, 1, 0, (uint64)&de, off, sizeof(de));
    }
}

/**
 * Allocate an entry on disk. Caller must hold dp->lock.
 */
struct dirent *ealloc(struct dirent *dp, char *name, int attr)
{
    if (!(dp->attribute & ATTR_DIRECTORY)) {
        panic("ealloc not dir");
    }
    if (dp->valid != 1 || !(name = formatname(name))) {        // detect illegal character
        return NULL;
    }
    struct dirent *ep;
    uint off = 0;
    if ((ep = dirlookup(dp, name, &off)) != 0) {      // entry exists
        return ep;
    }
    //如果不存在,获取一个dirent
    ep = eget(dp, name);
    elock(ep);
    ep->attribute = attr;
    ep->file_size = 0;
    ep->first_clus = 0;
    ep->parent = edup(dp);
    ep->off = off;
    ep->clus_cnt = 0;
    ep->cur_clus = 0;
    ep->dirty = 0;
    //复制文件名
    strncpy(ep->filename, name, FAT32_MAX_FILENAME);
    ep->filename[FAT32_MAX_FILENAME] = '\0';
    if (attr == ATTR_DIRECTORY) {    // generate "." and ".." for ep
        ep->attribute |= ATTR_DIRECTORY;
        ep->cur_clus = ep->first_clus = alloc_clus(dp->dev);
        emake(ep, ep, 0);
        emake(ep, dp, 32);
    } else {
        ep->attribute |= ATTR_ARCHIVE;
    }
    emake(dp, ep, off);
    ep->valid = 1;
    eunlock(ep);
    return ep;
}

// entry项引用次数+1
struct dirent *edup(struct dirent *entry)
{
    if (entry != 0) {
        acquire(&ecache.lock);
        entry->ref++;
        release(&ecache.lock);
    }
    return entry;
}

// Only update filesize and first cluster in this case.
// caller must hold entry->parent->lock
// 更新entry，只更新文件size和第一簇号
void eupdate(struct dirent *entry)
{
    if (!entry->dirty || entry->valid != 1) { return; }
    uint entcnt = 0;
    uint32 off = reloc_clus(entry->parent, entry->off, 0);
    rw_clus(entry->parent->cur_clus, 0, 0, (uint64) &entcnt, off, 1);
    entcnt &= ~LAST_LONG_ENTRY;
    off = reloc_clus(entry->parent, entry->off + (entcnt << 5), 0);
    union dentry de;
    rw_clus(entry->parent->cur_clus, 0, 0, (uint64)&de, off, sizeof(de));
    de.sne.fst_clus_hi = (uint16)(entry->first_clus >> 16);
    de.sne.fst_clus_lo = (uint16)(entry->first_clus & 0xffff);
    de.sne.file_size = entry->file_size;
    rw_clus(entry->parent->cur_clus, 1, 0, (uint64)&de, off, sizeof(de));
    entry->dirty = 0;
}

// caller must hold entry->lock
// caller must hold entry->parent->lock
// remove the entry in its parent directory
// 将此entry从目录中移除
void eremove(struct dirent *entry)
{
    if (entry->valid != 1) { return; }
    uint entcnt = 0;
    uint32 off = entry->off;
    uint32 off2 = reloc_clus(entry->parent, off, 0);
    rw_clus(entry->parent->cur_clus, 0, 0, (uint64) &entcnt, off2, 1);
    entcnt &= ~LAST_LONG_ENTRY;
    uint8 flag = EMPTY_ENTRY;
    for (int i = 0; i <= entcnt; i++) {
        rw_clus(entry->parent->cur_clus, 1, 0, (uint64) &flag, off2, 1);
        off += 32;
        off2 = reloc_clus(entry->parent, off, 0);
    }
    entry->valid = -1;
}

// truncate a file
// caller must hold entry->lock
// 截断文件
void etrunc(struct dirent *entry)
{
    for (uint32 clus = entry->first_clus; clus >= 2 && clus < FAT32_EOC; ) {
        uint32 next = read_fat(clus);
        free_clus(clus);
        clus = next;
    }
    entry->file_size = 0;
    entry->first_clus = 0;
    entry->dirty = 1;
}

// 上锁
void elock(struct dirent *entry)
{
    if (entry == 0 || entry->ref < 1)
        panic("elock");
    acquiresleep(&entry->lock);
}

// 解锁
void eunlock(struct dirent *entry)
{
    if (entry == 0 || !holdingsleep(&entry->lock) || entry->ref < 1)
        panic("eunlock");
    releasesleep(&entry->lock);
}


void eput(struct dirent *entry)
{
    acquire(&ecache.lock);
    if (entry != &root && entry->valid != 0 && entry->ref == 1) {
        // ref == 1 means no other process can have entry locked,
        // so this acquiresleep() won't block (or deadlock).
        acquiresleep(&entry->lock);
        entry->next->prev = entry->prev;
        entry->prev->next = entry->next;
        entry->next = root.next;
        entry->prev = &root;
        root.next->prev = entry;
        root.next = entry;
        release(&ecache.lock);
        if (entry->valid == -1) {       // this means some one has called eremove()
            etrunc(entry);
        } else {
            elock(entry->parent);
            eupdate(entry);
            eunlock(entry->parent);
        }
        releasesleep(&entry->lock);

        // Once entry->ref decreases down to 0, we can't guarantee the entry->parent field remains unchanged.
        // Because eget() may take the entry away and write it.
        struct dirent *eparent = entry->parent;
        acquire(&ecache.lock);
        entry->ref--;
        release(&ecache.lock);
        if (entry->ref == 0) {
            eput(eparent);
        }
        return;
    }
    entry->ref--;
    release(&ecache.lock);
}


// 把de中的内容复制到st中
void estat(struct dirent *de, struct stat *st)
{
    strncpy(st->name, de->filename, STAT_MAX_NAME);
    st->type = (de->attribute & ATTR_DIRECTORY) ? T_DIR : T_FILE;
    st->dev = de->dev;
    st->size = de->file_size;
}

/**
 * Read filename from directory entry.
 * @param   buffer      pointer to the array that stores the name
 * @param   raw_entry   pointer to the entry in a sector buffer
 * @param   islong      if non-zero, read as l-n-e, otherwise s-n-e.
 */
static void read_entry_name(char *buffer, union dentry *d)
{
    if (d->lne.attr == ATTR_LONG_NAME) {                       // long entry branch
        wchar temp[NELEM(d->lne.name1)];
        memmove(temp, d->lne.name1, sizeof(temp));
        snstr(buffer, temp, NELEM(d->lne.name1));
        buffer += NELEM(d->lne.name1);
        snstr(buffer, d->lne.name2, NELEM(d->lne.name2));
        buffer += NELEM(d->lne.name2);
        snstr(buffer, d->lne.name3, NELEM(d->lne.name3));
    } else {
        // assert: only "." and ".." will enter this branch
        memset(buffer, 0, CHAR_SHORT_NAME + 2); // plus '.' and '\0'
        int i;
        for (i = 0; d->sne.name[i] != ' ' && i < 8; i++) {
            buffer[i] = d->sne.name[i];
        }
        if (d->sne.name[8] != ' ') {
            buffer[i++] = '.';
        }
        for (int j = 8; j < CHAR_SHORT_NAME; j++, i++) {
            if (d->sne.name[j] == ' ') { break; }
            buffer[i] = d->sne.name[j];
        }
    }
}

/**
 * Read entry_info from directory entry.
 * @param   entry       pointer to the structure that stores the entry info
 * @param   raw_entry   pointer to the entry in a sector buffer
 */
static void read_entry_info(struct dirent *entry, union dentry *d)
{
    entry->attribute = d->sne.attr;
    entry->first_clus = ((uint32)d->sne.fst_clus_hi << 16) | d->sne.fst_clus_lo;
    entry->file_size = d->sne.file_size;
    entry->cur_clus = entry->first_clus;
    entry->clus_cnt = 0;
}

/**
 * Read a directory from off, parse the next entry(ies) associated with one file, or find empty entry slots.
 * Caller must hold dp->lock.
 * @param   dp      the directory
 * @param   ep      the struct to be written with info
 * @param   off     offset off the directory
 * @param   count   to write the count of entries
 * @return  -1      meet the end of dir
 *          0       find empty slots
 *          1       find a file with all its entries
 */
int enext(struct dirent *dp, struct dirent *ep, uint off, int *count)
{
    if (!(dp->attribute & ATTR_DIRECTORY))
        panic("enext not dir");
    if (ep->valid)
        panic("enext ep valid");
    if (off % 32)
        panic("enext not align");
    if (dp->valid != 1) { return -1; }

    union dentry de;
    int cnt = 0;
    memset(ep->filename, 0, FAT32_MAX_FILENAME + 1);
    for (int off2; (off2 = reloc_clus(dp, off, 0)) != -1; off += 32) {
        if (rw_clus(dp->cur_clus, 0, 0, (uint64)&de, off2, 32) != 32 || de.lne.order == END_OF_ENTRY) {
            return -1;
        }
        if (de.lne.order == EMPTY_ENTRY) {
            cnt++;
            continue;
        } else if (cnt) {
            *count = cnt;
            return 0;
        }
        if (de.lne.attr == ATTR_LONG_NAME) {
            int lcnt = de.lne.order & ~LAST_LONG_ENTRY;
            if (de.lne.order & LAST_LONG_ENTRY) {
                *count = lcnt + 1;                              // plus the s-n-e;
                count = 0;
            }
            read_entry_name(ep->filename + (lcnt - 1) * CHAR_LONG_NAME, &de);
        } else {
            if (count) {
                *count = 1;
                read_entry_name(ep->filename, &de);
            }
            read_entry_info(ep, &de);
            return 1;
        }
    }
    return -1;
}

/**
 * Seacher for the entry in a directory and return a structure. Besides, record the offset of
 * some continuous empty slots that can fit the length of filename.
 * Caller must hold entry->lock.
 * @param   dp          entry of a directory file
 * @param   filename    target filename
 * @param   poff        offset of proper empty entry slots from the beginning of the dir
 */
struct dirent *dirlookup(struct dirent *dp, char *filename, uint *poff)
{
    if (!(dp->attribute & ATTR_DIRECTORY))
        panic("dirlookup not DIR");
    if (strncmp(filename, ".", FAT32_MAX_FILENAME) == 0) {
        return edup(dp);
    } else if (strncmp(filename, "..", FAT32_MAX_FILENAME) == 0) {
        if (dp == &root) {
            return edup(&root);
        }
        return edup(dp->parent);
    }
    if (dp->valid != 1) {
        return NULL;
    }
    struct dirent *ep = eget(dp, filename);
    if (ep->valid == 1) { return ep; }                               // ecache hits

    int len = strlen(filename);
    int entcnt = (len + CHAR_LONG_NAME - 1) / CHAR_LONG_NAME + 1;   // count of l-n-entries, rounds up. plus s-n-e
    int count = 0;
    int type;
    uint off = 0;
    reloc_clus(dp, 0, 0);
    while ((type = enext(dp, ep, off, &count) != -1)) {
        if (type == 0) {
            if (poff && count >= entcnt) {
                *poff = off;
                poff = 0;
            }
        } else if (strncmp(filename, ep->filename, FAT32_MAX_FILENAME) == 0) {
            ep->parent = edup(dp);
            ep->off = off;
            ep->valid = 1;
            return ep;
        }
        off += count << 5;
    }
    if (poff) {
        *poff = off;
    }
    eput(ep);
    return NULL;
}

static char *skipelem(char *path, char *name)
{
    while (*path == '/') {
        path++;
    }
    if (*path == 0) { return NULL; }
    char *s = path;
    while (*path != '/' && *path != 0) {
        path++;
    }
    int len = path - s;
    if (len > FAT32_MAX_FILENAME) {
        len = FAT32_MAX_FILENAME;
    }
    name[len] = 0;
    memmove(name, s, len);
    while (*path == '/') {
        path++;
    }
    return path;
}

// FAT32 version of namex in xv6's original file system.
static struct dirent *lookup_path(char *path, int parent, char *name)
{
    struct dirent *entry, *next;
    if (*path == '/') {
        entry = edup(&root);
    } else if (*path != '\0') {
        entry = edup(myproc()->cwd);
    } else {
        return NULL;
    }
    while ((path = skipelem(path, name)) != 0) {
        elock(entry);
        if (!(entry->attribute & ATTR_DIRECTORY)) {
            eunlock(entry);
            eput(entry);
            return NULL;
        }
        if (parent && *path == '\0') {
            eunlock(entry);
            return entry;
        }
        if ((next = dirlookup(entry, name, 0)) == 0) {
            eunlock(entry);
            eput(entry);
            return NULL;
        }
        eunlock(entry);
        eput(entry);
        entry = next;
    }
    if (parent) {
        eput(entry);
        return NULL;
    }
    return entry;
}

struct dirent *ename(char *path)
{
    char name[FAT32_MAX_FILENAME + 1];
    return lookup_path(path, 0, name);
}

struct dirent *enameparent(char *path, char *name)
{
    return lookup_path(path, 1, name);
}
