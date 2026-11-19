// xbc.c — .xbc 字节码文件的读写
//
// 02-编译与运行时.md §2 的核心承诺是「两档共享同一份字节码」：
// AOT 档不是另一条前端路径，而是「对同一份字节码跑优化 pass」。
// 要让这个承诺能落地，字节码必须能**存盘再读回**，且读回后的
// 执行结果与直接编译完全一致。这就是本文件的全部职责。
//
// P0 的格式（刻意简单，够用就行）：
//
//   offset  size  内容
//   ------  ----  --------------------------------------------
//   0       4     魔数 "XBC1"（0x58424331，小端）
//   4       4     格式版本（与 Bytecode.version 一致）
//   8       4     常量池个数 nconst
//   12      4     函数个数 nfunc
//   16      4     指令条数 ncode
//   20      ...   常量池
//    ...    ...   函数原型
//    ...    ...   指令序列
//
// 为什么没有「单条指令的源码位置里的文件名」：P0 是单文件编译，
// 整个 .xbc 只有一个文件名，存在头部一次即可。多文件在 P1 引入
// 模块系统时再改（那时本来就要提升版本号）。
//
// 为什么不用「定长 6 字节指令」（02 §2 的设计）：那是寄存器式字节码
// 的目标形态，P0 是栈式 + 16 字节的 Insn 结构体。等 P1 换寄存器式时
// 一次性改成定长编码，同时提升版本号。

#include "x.h"
#include <errno.h>

#define XBC_MAGIC 0x58424331u   // "XBC1"

// ============================================================
// 写
// ============================================================

bool xbc_write(const char* path, Bytecode* bc, const char* src_file) {
    FILE* f = x_fopen(path, "wb");
    if (!f) {
        diag_error((SrcPos){ src_file, 0, 0 },
                   "无法创建输出文件 `%s`：%s", path, strerror(errno));
        return false;
    }

    uint32_t magic = XBC_MAGIC;
    int32_t ncode  = bc->code_count;
    int32_t nconst = bc->const_count;
    int32_t nfunc  = bc->func_count;

    fwrite(&magic,    sizeof(magic),    1, f);
    fwrite(&bc->version, sizeof(bc->version), 1, f);
    fwrite(&nconst,   sizeof(nconst),   1, f);
    fwrite(&nfunc,    sizeof(nfunc),    1, f);
    fwrite(&ncode,    sizeof(ncode),    1, f);

    // 源文件名（整个文件共用）
    int32_t flen = src_file ? (int32_t)strlen(src_file) : 0;
    fwrite(&flen, sizeof(flen), 1, f);
    if (flen) fwrite(src_file, 1, (size_t)flen, f);

    // ---- 结构体类型表 ----
    //
    // 放在常量池之前：读的时候类型表必须先于指令可用，而指令里
    // 的 BC_NEW_STRUCT / BC_FIELD_GET 直接用类型索引。放最后
    // 也能读（读是一次性完成的），但放前面能让「类型表损坏」
    // 更早失败，不会先花时间读完所有指令。
    {
        int32_t nstruct = bc->struct_count;
        fwrite(&nstruct, sizeof(nstruct), 1, f);
        for (int i = 0; i < nstruct; i++) {
            StructType* st = &bc->structs[i];
            int32_t nl = st->name ? (int32_t)strlen(st->name) : 0;
            fwrite(&nl, sizeof(nl), 1, f);
            if (nl) fwrite(st->name, 1, (size_t)nl, f);
            fwrite(&st->field_count, sizeof(st->field_count), 1, f);
            for (int k = 0; k < st->field_count; k++) {
                int32_t fnl = st->field_names[k] ? (int32_t)strlen(st->field_names[k]) : 0;
                fwrite(&fnl, sizeof(fnl), 1, f);
                if (fnl) fwrite(st->field_names[k], 1, (size_t)fnl, f);
                int32_t ftl = st->field_types[k] ? (int32_t)strlen(st->field_types[k]) : 0;
                fwrite(&ftl, sizeof(ftl), 1, f);
                if (ftl) fwrite(st->field_types[k], 1, (size_t)ftl, f);
            }
        }
    }

    // ---- 方法槽表 + 类表 ----
    //
    // 为什么槽表要单独存：虚表是按**全局方法名槽号**索引的，运行时
    // 靠槽号找实现。如果只存每个类的虚表数组而不存「下标 → 方法名」
    // 的映射，运行时就只能靠数字猜 —— 报错信息里连方法名都写不出来。
    //
    // 槽表在类表之前：读类表时要用它校验虚表长度是否与槽数一致
    // （不一致说明文件被改过或版本错配，能立刻失败而不是读到一半
    // 才越界）。
    {
        int32_t nslot = bc->method_slot_count;
        fwrite(&nslot, sizeof(nslot), 1, f);
        for (int i = 0; i < nslot; i++) {
            const char* mn = bc->method_slots[i].name;
            int32_t ml = mn ? (int32_t)strlen(mn) : 0;
            fwrite(&ml, sizeof(ml), 1, f);
            if (ml) fwrite(mn, 1, (size_t)ml, f);
        }
    }

    {
        int32_t nclass = bc->class_count;
        fwrite(&nclass, sizeof(nclass), 1, f);
        for (int i = 0; i < nclass; i++) {
            ClassType* ct = &bc->classes[i];

            int32_t nl = ct->name ? (int32_t)strlen(ct->name) : 0;
            fwrite(&nl, sizeof(nl), 1, f);
            if (nl) fwrite(ct->name, 1, (size_t)nl, f);

            fwrite(&ct->parent_idx,      sizeof(ct->parent_idx),      1, f);
            fwrite(&ct->ctor_fn,         sizeof(ct->ctor_fn),         1, f);
            fwrite(&ct->field_count,     sizeof(ct->field_count),     1, f);
            fwrite(&ct->own_field_start, sizeof(ct->own_field_start), 1, f);

            // 三个布尔标志按 int32 存 —— 磁盘格式不依赖 bool 的大小
            // 与填充，换编译器也不会读错。
            int32_t b_abs = ct->is_abstract  ? 1 : 0;
            int32_t b_if  = ct->is_interface ? 1 : 0;
            int32_t b_fin = ct->is_final     ? 1 : 0;
            fwrite(&b_abs, sizeof(b_abs), 1, f);
            fwrite(&b_if,  sizeof(b_if),  1, f);
            fwrite(&b_fin, sizeof(b_fin), 1, f);

            // 字段：名字 + 类型名 + 可见性 + final
            for (int k = 0; k < ct->field_count; k++) {
                const char* fn = ct->field_names ? ct->field_names[k] : NULL;
                int32_t fnl = fn ? (int32_t)strlen(fn) : 0;
                fwrite(&fnl, sizeof(fnl), 1, f);
                if (fnl) fwrite(fn, 1, (size_t)fnl, f);

                const char* ft = ct->field_types ? ct->field_types[k] : NULL;
                int32_t ftl = ft ? (int32_t)strlen(ft) : 0;
                fwrite(&ftl, sizeof(ftl), 1, f);
                if (ftl) fwrite(ft, 1, (size_t)ftl, f);

                int32_t vis = ct->field_vis ? (int32_t)ct->field_vis[k] : 0;
                int32_t fin = (ct->field_final && ct->field_final[k]) ? 1 : 0;
                fwrite(&vis, sizeof(vis), 1, f);
                fwrite(&fin, sizeof(fin), 1, f);
            }

            // 虚表
            fwrite(&ct->vtable_len, sizeof(ct->vtable_len), 1, f);
            for (int k = 0; k < ct->vtable_len; k++) {
                int32_t v = ct->vtable ? ct->vtable[k] : -1;
                fwrite(&v, sizeof(v), 1, f);
            }

            // 方法可见性（与虚表平行）。反射要读它 —— 没有它，
            // 加载回来的 .xbc 里所有方法都变成 public，封装在
            // 「发一份字节码给别人」这条路上直接失效。
            for (int k = 0; k < ct->vtable_len; k++) {
                int32_t v = ct->method_vis ? (int32_t)ct->method_vis[k] : 0;
                fwrite(&v, sizeof(v), 1, f);
            }

            // 接口索引表
            fwrite(&ct->iface_count, sizeof(ct->iface_count), 1, f);
            for (int k = 0; k < ct->iface_count; k++) {
                int32_t v = ct->iface_idx ? ct->iface_idx[k] : -1;
                fwrite(&v, sizeof(v), 1, f);
            }
        }
    }

    // ---- 注解元数据 ----
    //
    // 放在类表之后：注解记录用 class_idx 指向类，读的时候类表必须
    // 已经就绪才能校验索引。放在常量池之前是随意的，但这个位置
    // 让「所有类型相关的段」连成一片，读代码时不必来回跳。
    //
    // 每条记录自带 target/member，所以不需要按类分组 —— 这是扁平
    // 表的好处：读写各一套循环，加一个目标类型不用改格式。
    {
        int32_t nanno = bc->anno_count;
        fwrite(&nanno, sizeof(nanno), 1, f);
        for (int i = 0; i < nanno; i++) {
            AnnoEntry* e = &bc->annos[i];
            int32_t tgt = (int32_t)e->target;
            fwrite(&tgt, sizeof(tgt), 1, f);
            fwrite(&e->class_idx, sizeof(e->class_idx), 1, f);

            int32_t ml = e->member ? (int32_t)strlen(e->member) : 0;
            fwrite(&ml, sizeof(ml), 1, f);
            if (ml) fwrite(e->member, 1, (size_t)ml, f);

            int32_t nl = e->name ? (int32_t)strlen(e->name) : 0;
            fwrite(&nl, sizeof(nl), 1, f);
            if (nl) fwrite(e->name, 1, (size_t)nl, f);

            // 实参。键与值是两个平行数组，键可以为空（位置实参）——
            // 用长度 0 表示「没有键」，读回来是 NULL。
            fwrite(&e->arg_count, sizeof(e->arg_count), 1, f);
            for (int k = 0; k < e->arg_count; k++) {
                const char* key = (e->arg_keys && e->arg_keys[k])
                                  ? e->arg_keys[k] : NULL;
                int32_t kl = key ? (int32_t)strlen(key) : 0;
                fwrite(&kl, sizeof(kl), 1, f);
                if (kl) fwrite(key, 1, (size_t)kl, f);

                const char* val = (e->arg_vals && e->arg_vals[k])
                                  ? e->arg_vals[k] : "";
                int32_t vl = (int32_t)strlen(val);
                fwrite(&vl, sizeof(vl), 1, f);
                if (vl) fwrite(val, 1, (size_t)vl, f);
            }
        }
    }

    // ---- 常量池 ----
    for (int i = 0; i < nconst; i++) {
        Constant* c = &bc->consts[i];
        int32_t kind = (int32_t)c->kind;
        fwrite(&kind, sizeof(kind), 1, f);
        switch (c->kind) {
            case CONST_I64:
                fwrite(&c->i, sizeof(c->i), 1, f);
                break;
            case CONST_F64:
                fwrite(&c->f, sizeof(c->f), 1, f);
                break;
            case CONST_STR: {
                int32_t len = (int32_t)c->str.len;
                fwrite(&len, sizeof(len), 1, f);
                if (len) fwrite(c->str.s, 1, (size_t)len, f);
                break;
            }
        }
    }

    // ---- 函数原型 ----
    for (int i = 0; i < nfunc; i++) {
        FuncProto* fn = &bc->funcs[i];
        int32_t name_len = fn->name ? (int32_t)strlen(fn->name) : 0;
        fwrite(&name_len, sizeof(name_len), 1, f);
        if (name_len) fwrite(fn->name, 1, (size_t)name_len, f);
        fwrite(&fn->param_count, sizeof(fn->param_count), 1, f);

        // 参数名 / 参数类型名（v0.20，格式 0x0008）。
        //
        // methodParams 反射要靠它做「按名绑定」。param_count 为 0 时
        // 不写任何内容 —— 大多数函数（内建包装、main）没有参数，
        // 空段落白占字节。读侧以 param_count 为准，两半永远对称。
        for (int k = 0; k < fn->param_count; k++) {
            const char* pn = fn->param_names ? fn->param_names[k] : NULL;
            int32_t pnl = pn ? (int32_t)strlen(pn) : 0;
            fwrite(&pnl, sizeof(pnl), 1, f);
            if (pnl) fwrite(pn, 1, (size_t)pnl, f);

            const char* pt = fn->param_types ? fn->param_types[k] : NULL;
            int32_t ptl = pt ? (int32_t)strlen(pt) : 0;
            fwrite(&ptl, sizeof(ptl), 1, f);
            if (ptl) fwrite(pt, 1, (size_t)ptl, f);
        }

        fwrite(&fn->local_count, sizeof(fn->local_count), 1, f);
        fwrite(&fn->code_start,  sizeof(fn->code_start),  1, f);
        fwrite(&fn->code_len,    sizeof(fn->code_len),    1, f);
        fwrite(&fn->pos.line,    sizeof(fn->pos.line),    1, f);
        fwrite(&fn->pos.col,     sizeof(fn->pos.col),     1, f);
    }

    // ---- 指令 ----
    // 注意：磁盘格式与内存布局**故意不同**。内存里指令是紧凑的 8 字节
    // （追求 cache 友好），磁盘上仍然逐条写完整信息（追求格式稳定、
    // 便于不用工具直接看）。这样内存布局的后续优化不会牵动文件格式。
    for (int i = 0; i < ncode; i++) {
        Insn* in = &bc->code[i];
        SrcPos p = bc->pos[i];
        int32_t op = (int32_t)in->op;
        int32_t a  = (int32_t)in->a;
        int32_t b  = (int32_t)in->b;
        int32_t c  = (int32_t)in->c;
        fwrite(&op, sizeof(op), 1, f);
        fwrite(&a,  sizeof(a),  1, f);
        fwrite(&b,  sizeof(b),  1, f);
        // c 必须落盘。它承载需要第三个操作数的超指令
        // （BC_LOAD_ADD_STORE 的槽位、BC_BUILTIN 的 math 子操作编码）。
        // 漏掉它的症状很隐蔽：算术与 IO 全部正常，只有用到 c 的指令
        // 在 x run 与 x run .xbc 两档下行为不同 —— 而普通测试只跑
        // 前一条路径，发现不了。
        fwrite(&c,  sizeof(c),  1, f);
        fwrite(&p.line, sizeof(p.line), 1, f);
        fwrite(&p.col,  sizeof(p.col),  1, f);
    }

    fclose(f);
    return true;
}

// ============================================================
// 读
// ============================================================
// 校验做三件事，每件都对应一类真实故障：
//   1. 魔数不符  → 用户传了个别的文件（如 .xbc 旧版本或根本不是字节码）
//   2. 版本不符  → 字节码格式变了，必须重编译而不是猜
//   3. 长度不足  → 文件被截断（下载中断、磁盘满）

bool xbc_read(const char* path, Bytecode* out) {
    FILE* f = x_fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: 无法打开 `%s`：%s\n", path, strerror(errno));
        return false;
    }

    // 取文件长度，用于「截断检查」的粗判
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t magic = 0;
    uint32_t version = 0;
    int32_t nconst = 0, nfunc = 0, ncode = 0;

    if (fread(&magic, sizeof(magic), 1, f) != 1) goto truncated;
    if (magic != XBC_MAGIC) {
        fprintf(stderr,
                "error: `%s` 不是 X 字节码文件\n"
                "  note: 文件头应为 \"XBC1\"，实际读到 0x%08X\n"
                "  help: .xbc 由 `x build xxx.xle` 生成；若这是源码请用 `x run xxx.xle`\n",
                path, magic);
        fclose(f);
        return false;
    }

    if (fread(&version, sizeof(version), 1, f) != 1) goto truncated;
    if (version != XBC_VERSION) {
        fprintf(stderr,
                "error: `%s` 的字节码版本为 0x%04X，当前编译器支持 0x%04X\n"
                "  note: 字节码格式在编译器升级后不保证兼容\n"
                "  help: 用当前版本的编译器重新生成：`x build <源文件>`\n",
                path, version, (unsigned)XBC_VERSION);
        fclose(f);
        return false;
    }

    if (fread(&nconst, sizeof(nconst), 1, f) != 1) goto truncated;
    if (fread(&nfunc,  sizeof(nfunc),  1, f) != 1) goto truncated;
    if (fread(&ncode,  sizeof(ncode),  1, f) != 1) goto truncated;

    // 合理性检查：防止损坏的长度字段导致巨额 malloc
    if (nconst < 0 || nconst > (1 << 24) ||
        nfunc  < 0 || nfunc  > (1 << 20) ||
        ncode  < 0 || ncode  > (1 << 26)) {
        fprintf(stderr,
                "error: `%s` 的头部数据不合理（常量 %d / 函数 %d / 指令 %d）\n"
                "  help: 文件可能已损坏，请重新编译\n",
                path, nconst, nfunc, ncode);
        fclose(f);
        return false;
    }

    int32_t flen = 0;
    if (fread(&flen, sizeof(flen), 1, f) != 1) goto truncated;
    char* src_file = NULL;
    if (flen > 0) {
        if (flen > 4096) goto truncated;
        src_file = x_alloc((size_t)flen + 1);
        if (fread(src_file, 1, (size_t)flen, f) != (size_t)flen) {
            free(src_file);
            goto truncated;
        }
        src_file[flen] = '\0';
    }

    bc_init(out);
    out->version = version;

    // ---- 结构体类型表 ----
    {
        int32_t nstruct = 0;
        if (fread(&nstruct, sizeof(nstruct), 1, f) != 1) goto truncated;
        if (nstruct < 0 || nstruct > (1 << 16)) {
            fprintf(stderr, "error: `%s` 的结构体类型表长度不合理（%d）\n"
                            "  help: 文件可能已损坏，请重新编译\n", path, nstruct);
            fclose(f);
            free(src_file);
            return false;
        }
        for (int i = 0; i < nstruct; i++) {
            int32_t nl = 0;
            if (fread(&nl, sizeof(nl), 1, f) != 1) goto truncated;
            if (nl < 0 || nl > 4096) goto truncated;
            char* sname = x_alloc((size_t)nl + 1);
            if (nl > 0 && fread(sname, 1, (size_t)nl, f) != (size_t)nl) {
                free(sname);
                goto truncated;
            }
            sname[nl] = '\0';

            int32_t fc = 0;
            if (fread(&fc, sizeof(fc), 1, f) != 1) { free(sname); goto truncated; }
            if (fc < 0 || fc > 4096) { free(sname); goto truncated; }

            char** fnames = fc ? x_alloc((size_t)fc * sizeof(char*)) : NULL;
            char** ftypes = fc ? x_alloc((size_t)fc * sizeof(char*)) : NULL;
            bool bad = false;
            for (int k = 0; k < fc && !bad; k++) {
                fnames[k] = NULL;
                ftypes[k] = NULL;

                int32_t fnl = 0;
                if (fread(&fnl, sizeof(fnl), 1, f) != 1) { bad = true; break; }
                if (fnl < 0 || fnl > 4096) { bad = true; break; }
                char* fn = x_alloc((size_t)fnl + 1);
                if (fnl > 0 && fread(fn, 1, (size_t)fnl, f) != (size_t)fnl) {
                    free(fn); bad = true; break;
                }
                fn[fnl] = '\0';
                fnames[k] = fn;

                int32_t ftl = 0;
                if (fread(&ftl, sizeof(ftl), 1, f) != 1) { bad = true; break; }
                if (ftl < 0 || ftl > 4096) { bad = true; break; }
                char* ft = x_alloc((size_t)ftl + 1);
                if (ftl > 0 && fread(ft, 1, (size_t)ftl, f) != (size_t)ftl) {
                    free(ft); bad = true; break;
                }
                ft[ftl] = '\0';
                ftypes[k] = ft;
            }

            if (bad) {
                for (int k = 0; k < fc; k++) {
                    if (fnames && fnames[k]) free(fnames[k]);
                    if (ftypes && ftypes[k]) free(ftypes[k]);
                }
                free(fnames); free(ftypes);
                free(sname);
                goto truncated;
            }

            // bc_add_struct 内部会 x_strdup，所以这些临时缓冲可以释放
            bc_add_struct(out, sname, fnames, ftypes, fc);
            for (int k = 0; k < fc; k++) { free(fnames[k]); free(ftypes[k]); }
            free(fnames); free(ftypes);
            free(sname);
        }
    }

    // ---- 方法槽表 + 类表 ----
    {
        int32_t nslot = 0;
        if (fread(&nslot, sizeof(nslot), 1, f) != 1) goto truncated;
        if (nslot < 0 || nslot > (1 << 16)) {
            fprintf(stderr, "error: `%s` 的方法槽表长度不合理（%d）\n"
                            "  help: 文件可能已损坏，请重新编译\n", path, nslot);
            fclose(f);
            free(src_file);
            return false;
        }
        for (int i = 0; i < nslot; i++) {
            int32_t ml = 0;
            if (fread(&ml, sizeof(ml), 1, f) != 1) goto truncated;
            if (ml < 0 || ml > 4096) goto truncated;
            char* mn = x_alloc((size_t)ml + 1);
            if (ml > 0 && fread(mn, 1, (size_t)ml, f) != (size_t)ml) {
                free(mn);
                goto truncated;
            }
            mn[ml] = '\0';
            // bc_method_slot 内部 x_strdup，且它会按名字去重
            bc_method_slot(out, mn);
            free(mn);
        }
    }

    {
        int32_t nclass = 0;
        if (fread(&nclass, sizeof(nclass), 1, f) != 1) goto truncated;
        if (nclass < 0 || nclass > (1 << 16)) {
            fprintf(stderr, "error: `%s` 的类表长度不合理（%d）\n"
                            "  help: 文件可能已损坏，请重新编译\n", path, nclass);
            fclose(f);
            free(src_file);
            return false;
        }

        for (int i = 0; i < nclass; i++) {
            int32_t nl = 0;
            if (fread(&nl, sizeof(nl), 1, f) != 1) goto truncated;
            if (nl < 0 || nl > 4096) goto truncated;
            char* cname = x_alloc((size_t)nl + 1);
            if (nl > 0 && fread(cname, 1, (size_t)nl, f) != (size_t)nl) {
                free(cname);
                goto truncated;
            }
            cname[nl] = '\0';

            int32_t parent_idx = -1, ctor_fn = -1, fcount = 0, own_start = 0;
            if (fread(&parent_idx, sizeof(parent_idx), 1, f) != 1 ||
                fread(&ctor_fn,    sizeof(ctor_fn),    1, f) != 1 ||
                fread(&fcount,     sizeof(fcount),     1, f) != 1 ||
                fread(&own_start,  sizeof(own_start),  1, f) != 1) {
                free(cname);
                goto truncated;
            }
            if (fcount < 0 || fcount > 4096) { free(cname); goto truncated; }

            int32_t b_abs = 0, b_if = 0, b_fin = 0;
            if (fread(&b_abs, sizeof(b_abs), 1, f) != 1 ||
                fread(&b_if,  sizeof(b_if),  1, f) != 1 ||
                fread(&b_fin, sizeof(b_fin), 1, f) != 1) {
                free(cname);
                goto truncated;
            }

            // 用 bc_add_class 建壳（它负责清零与默认值），再逐字段填。
            //
            // ── 不能直接写 bc->classes[n] ──────────────────────────
            //
            // bc_add_class 会做 memset + 设 parent_idx = -1 / ctor_fn = -1
            // 这些默认值。绕过它去手写结构体，等于把「默认值」这件事
            // 复制到第二处，将来加字段就会漏。
            int ci = bc_add_class(out, cname);
            ClassType* ct = &out->classes[ci];
            ct->parent_idx      = parent_idx;
            ct->ctor_fn         = ctor_fn;
            ct->own_field_start = own_start;
            ct->is_abstract  = (b_abs != 0);
            ct->is_interface = (b_if  != 0);
            ct->is_final     = (b_fin != 0);

            // 字段
            if (fcount > 0) {
                ct->field_count = fcount;
                ct->field_names = x_alloc((size_t)fcount * sizeof(char*));
                ct->field_types = x_alloc((size_t)fcount * sizeof(char*));
                ct->field_vis   = x_alloc((size_t)fcount * sizeof(Visibility));
                ct->field_final = x_alloc((size_t)fcount * sizeof(bool));

                for (int k = 0; k < fcount; k++) {
                    ct->field_names[k] = NULL;
                    ct->field_types[k] = NULL;
                    ct->field_vis[k]   = VIS_PUBLIC;
                    ct->field_final[k] = false;

                    int32_t fnl = 0;
                    if (fread(&fnl, sizeof(fnl), 1, f) != 1) { free(cname); goto truncated; }
                    if (fnl < 0 || fnl > 4096) { free(cname); goto truncated; }
                    char* fn = x_alloc((size_t)fnl + 1);
                    if (fnl > 0 && fread(fn, 1, (size_t)fnl, f) != (size_t)fnl) {
                        free(fn); free(cname); goto truncated;
                    }
                    fn[fnl] = '\0';
                    ct->field_names[k] = fn;   // 直接接管，不 dup

                    int32_t ftl = 0;
                    if (fread(&ftl, sizeof(ftl), 1, f) != 1) { free(cname); goto truncated; }
                    if (ftl < 0 || ftl > 4096) { free(cname); goto truncated; }
                    char* ft = x_alloc((size_t)ftl + 1);
                    if (ftl > 0 && fread(ft, 1, (size_t)ftl, f) != (size_t)ftl) {
                        free(ft); free(cname); goto truncated;
                    }
                    ft[ftl] = '\0';
                    ct->field_types[k] = ft;

                    int32_t vis = 0, fin = 0;
                    if (fread(&vis, sizeof(vis), 1, f) != 1 ||
                        fread(&fin, sizeof(fin), 1, f) != 1) {
                        free(cname);
                        goto truncated;
                    }
                    // 越界的可见性值按 public 处理 —— 它只影响诊断严格
                    // 程度，不该让一个坏字节变成崩溃。
                    ct->field_vis[k]   = (vis >= 0 && vis <= 2)
                                         ? (Visibility)vis : VIS_PUBLIC;
                    ct->field_final[k] = (fin != 0);
                }
            }

            // 虚表
            int32_t vlen = 0;
            if (fread(&vlen, sizeof(vlen), 1, f) != 1) { free(cname); goto truncated; }
            if (vlen < 0 || vlen > (1 << 16)) { free(cname); goto truncated; }
            ct->vtable_len = vlen;
            ct->vtable = vlen ? x_alloc((size_t)vlen * sizeof(int)) : NULL;
            for (int k = 0; k < vlen; k++) {
                int32_t v = -1;
                if (fread(&v, sizeof(v), 1, f) != 1) { free(cname); goto truncated; }
                // ── 这里**不能**拿 func_count 判范围 ────────────────
                //
                // 类表在磁盘上排在函数原型**之前**，此刻 out->func_count
                // 还是 0。用 `v < func_count` 判的话，所有正常索引都会
                // 被判越界、统一清成 -1 —— 症状是「所有类都没有实现
                // 任何方法」，而 .xbc 文件本身完全正确。
                //
                // 真正的范围校验放在函数原型读完之后（见下面的
                // 「虚表的函数索引校验」）。这里只挡明显不合理的值。
                ct->vtable[k] = (v >= -1 && v < (1 << 20)) ? (int)v : -1;
            }

            // 方法可见性（与虚表平行）。长度必须与 vtable_len 一致 ——
            // 不一致说明文件损坏，继续读下去只会把接口表错位。
            //
            // 越界值按 public 处理，理由与字段可见性相同：它只影响
            // 封装的严格程度，不该让一个坏字节变成加载失败。
            ct->method_vis = vlen ? x_alloc((size_t)vlen * sizeof(Visibility)) : NULL;
            for (int k = 0; k < vlen; k++) {
                int32_t v = 0;
                if (fread(&v, sizeof(v), 1, f) != 1) { free(cname); goto truncated; }
                ct->method_vis[k] = (v >= 0 && v <= 2)
                                    ? (Visibility)v : VIS_PUBLIC;
            }

            // 接口索引 —— 此时可能还没读完所有类，索引只做范围粗查。
            int32_t ic = 0;
            if (fread(&ic, sizeof(ic), 1, f) != 1) { free(cname); goto truncated; }
            if (ic < 0 || ic > 256) { free(cname); goto truncated; }
            ct->iface_count = ic;
            ct->iface_idx = ic ? x_alloc((size_t)ic * sizeof(int)) : NULL;
            for (int k = 0; k < ic; k++) {
                int32_t v = -1;
                if (fread(&v, sizeof(v), 1, f) != 1) { free(cname); goto truncated; }
                ct->iface_idx[k] = (int)v;
            }

            free(cname);
        }

        // ---- 虚表的函数索引宽度校验 ----
        //
        // 上面读虚表时用的是 out->func_count，但那一刻函数表还没读
        // （类表在函数原型之前）。所以这里补一次校验：读完全部函数
        // 原型之后再确认索引没越界，越界的统一清成 -1。
        //
        // 这段放在函数原型之后（见下面），这里只留标记。
    }

    // ---- 注解元数据 ----
    {
        int32_t nanno = 0;
        if (fread(&nanno, sizeof(nanno), 1, f) != 1) goto truncated;
        // 上限只是防损坏文件导致的巨额 malloc。注解数是「几十条」的
        // 量级，把上界设成 1<<16 已经宽松到不可能被正常文件撞到。
        if (nanno < 0 || nanno > (1 << 16)) goto truncated;

        for (int i = 0; i < nanno; i++) {
            int32_t tgt = 0;
            int32_t ci  = -1;
            if (fread(&tgt, sizeof(tgt), 1, f) != 1 ||
                fread(&ci,  sizeof(ci),  1, f) != 1) goto truncated;

            // 坏字节不要变成崩溃：越界的 target 把这条记录完整读掉
            // 再丢（不能只跳过错误字段 —— 那样字节流会全部错位，
            // 后果远比丢一条注解严重）。
            bool valid_target = (tgt >= 0 && tgt <= 2);

            char* member = NULL;
            int32_t ml = 0;
            if (fread(&ml, sizeof(ml), 1, f) != 1) goto truncated;
            if (ml < 0 || ml > 4096) goto truncated;
            if (ml > 0) {
                member = x_alloc((size_t)ml + 1);
                if (fread(member, 1, (size_t)ml, f) != (size_t)ml) {
                    free(member);
                    goto truncated;
                }
                member[ml] = '\0';
            }

            char* aname = NULL;
            int32_t nl = 0;
            if (fread(&nl, sizeof(nl), 1, f) != 1) { free(member); goto truncated; }
            if (nl < 0 || nl > 4096) { free(member); goto truncated; }
            if (nl > 0) {
                aname = x_alloc((size_t)nl + 1);
                if (fread(aname, 1, (size_t)nl, f) != (size_t)nl) {
                    free(aname); free(member);
                    goto truncated;
                }
                aname[nl] = '\0';
            }

            int32_t ac = 0;
            if (fread(&ac, sizeof(ac), 1, f) != 1) {
                free(aname); free(member);
                goto truncated;
            }
            if (ac < 0 || ac > 256) { free(aname); free(member); goto truncated; }

            // 直接按目标容量一次性建好记录，再逐项填 —— 走 bc_add_anno
            // 的话得先把实参拼成 Annotation，多一层无用的临时结构。
            AnnoEntry* e = NULL;
            if (valid_target) {
                if (out->anno_count == out->anno_cap) {
                    int nc = out->anno_cap ? out->anno_cap * 2 : 16;
                    AnnoEntry* ne = x_alloc((size_t)nc * sizeof(AnnoEntry));
                    if (out->annos) memcpy(ne, out->annos,
                                           (size_t)out->anno_count * sizeof(AnnoEntry));
                    out->annos = ne;
                    out->anno_cap = nc;
                }
                e = &out->annos[out->anno_count++];
                memset(e, 0, sizeof(*e));
                e->target    = (AnnoTarget)tgt;
                e->class_idx = (int)ci;
                e->member    = member;
                e->name      = aname ? aname : x_strdup("");
                e->arg_count = (int)ac;
                if (ac > 0) {
                    e->arg_keys = x_alloc((size_t)ac * sizeof(char*));
                    e->arg_vals = x_alloc((size_t)ac * sizeof(char*));
                }
            } else {
                free(member);
                free(aname);
            }

            for (int k = 0; k < ac; k++) {
                int32_t kl = 0;
                if (fread(&kl, sizeof(kl), 1, f) != 1) goto truncated;
                if (kl < 0 || kl > 4096) goto truncated;
                char* key = NULL;
                if (kl > 0) {
                    key = x_alloc((size_t)kl + 1);
                    if (fread(key, 1, (size_t)kl, f) != (size_t)kl) {
                        free(key);
                        goto truncated;
                    }
                    key[kl] = '\0';
                }

                int32_t vl = 0;
                if (fread(&vl, sizeof(vl), 1, f) != 1) { free(key); goto truncated; }
                if (vl < 0 || vl > (1 << 20)) { free(key); goto truncated; }
                char* val = x_alloc((size_t)vl + 1);
                if (vl > 0 && fread(val, 1, (size_t)vl, f) != (size_t)vl) {
                    free(val); free(key);
                    goto truncated;
                }
                val[vl] = '\0';

                if (e) {
                    e->arg_keys[k] = key;
                    e->arg_vals[k] = val;
                } else {
                    free(key);
                    free(val);
                }
            }
        }
    }

    // ---- 常量池 ----
    for (int i = 0; i < nconst; i++) {
        int32_t kind = 0;
        if (fread(&kind, sizeof(kind), 1, f) != 1) goto truncated;

        switch ((ConstKind)kind) {
            case CONST_I64: {
                int64_t v;
                if (fread(&v, sizeof(v), 1, f) != 1) goto truncated;
                bc_add_const_i64(out, v);   // 走统一入口，保持去重语义
                break;
            }
            case CONST_F64: {
                double v;
                if (fread(&v, sizeof(v), 1, f) != 1) goto truncated;
                bc_add_const_f64(out, v);
                break;
            }
            case CONST_STR: {
                int32_t len = 0;
                if (fread(&len, sizeof(len), 1, f) != 1) goto truncated;
                if (len < 0 || len > (1 << 24)) goto truncated;
                char* buf = x_alloc((size_t)len + 1);
                if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
                    free(buf);
                    goto truncated;
                }
                buf[len] = '\0';
                bc_add_const_str(out, buf, (size_t)len);
                free(buf);
                break;
            }
            default:
                fprintf(stderr, "error: `%s` 常量池第 %d 项类型未知（%d）\n", path, i, kind);
                fclose(f);
                free(src_file);
                return false;
        }
    }

    // ---- 函数原型 ----
    for (int i = 0; i < nfunc; i++) {
        int32_t name_len = 0;
        if (fread(&name_len, sizeof(name_len), 1, f) != 1) goto truncated;
        if (name_len < 0 || name_len > 4096) goto truncated;
        char* name = x_alloc((size_t)name_len + 1);
        if (name_len > 0 && fread(name, 1, (size_t)name_len, f) != (size_t)name_len) {
            free(name);
            goto truncated;
        }
        name[name_len] = '\0';

        int32_t param_count = 0, local_count = 0, code_start = 0, code_len = 0;
        int32_t line = 0, col = 0;
        if (fread(&param_count, sizeof(param_count), 1, f) != 1) goto truncated;

        // 参数名 / 参数类型名（与写侧对称，见 bc_write 的注释）。
        char** pnames = NULL;
        char** ptypes = NULL;
        if (param_count > 0) {
            pnames = x_alloc((size_t)param_count * sizeof(char*));
            ptypes = x_alloc((size_t)param_count * sizeof(char*));
            for (int k = 0; k < param_count; k++) {
                int32_t nl = 0;
                if (fread(&nl, sizeof(nl), 1, f) != 1 || nl < 0 || nl > 4096)
                    goto truncated;
                pnames[k] = NULL;
                if (nl > 0) {
                    pnames[k] = x_alloc((size_t)nl + 1);
                    if (fread(pnames[k], 1, (size_t)nl, f) != (size_t)nl)
                        goto truncated;
                    pnames[k][nl] = '\0';
                }
                int32_t tl = 0;
                if (fread(&tl, sizeof(tl), 1, f) != 1 || tl < 0 || tl > 4096)
                    goto truncated;
                ptypes[k] = NULL;
                if (tl > 0) {
                    ptypes[k] = x_alloc((size_t)tl + 1);
                    if (fread(ptypes[k], 1, (size_t)tl, f) != (size_t)tl)
                        goto truncated;
                    ptypes[k][tl] = '\0';
                }
            }
        }

        if (fread(&local_count, sizeof(local_count), 1, f) != 1 ||
            fread(&code_start,  sizeof(code_start),  1, f) != 1 ||
            fread(&code_len,    sizeof(code_len),    1, f) != 1 ||
            fread(&line,        sizeof(line),        1, f) != 1 ||
            fread(&col,         sizeof(col),         1, f) != 1) {
            goto truncated;
        }

        int idx = bc_add_func(out, name, param_count, (SrcPos){ src_file, line, col });
        out->funcs[idx].local_count = local_count;
        out->funcs[idx].code_start  = code_start;
        out->funcs[idx].code_len    = code_len;
        out->funcs[idx].param_names = pnames;
        out->funcs[idx].param_types = ptypes;
        free(name);
    }

    // ---- 虚表的函数索引校验 ----
    //
    // 读类表时函数原型还没读，所以那时无法判断「虚表里的索引是否
    // 指向一个真实函数」。现在 func_count 已经是终值，补一次。
    //
    // 越界的索引清成 -1（= 未实现）而不是报错退出：它只可能来自
    // 损坏的文件或版本错配，而运行时的「类 X 没有实现方法 Y」是
    // 一条能看懂的错，比在加载阶段崩掉有用。
    for (int i = 0; i < out->class_count; i++) {
        ClassType* ct = &out->classes[i];
        for (int k = 0; k < ct->vtable_len; k++) {
            if (ct->vtable[k] >= out->func_count) ct->vtable[k] = -1;
        }
        if (ct->parent_idx >= out->class_count) ct->parent_idx = -1;
        if (ct->ctor_fn   >= out->func_count) ct->ctor_fn   = -1;
        for (int k = 0; k < ct->iface_count; k++) {
            if (ct->iface_idx[k] >= out->class_count) ct->iface_idx[k] = -1;
        }
    }

    // ---- 指令 ----
    for (int i = 0; i < ncode; i++) {
        int32_t op = 0, a = 0, b = 0, c = 0, line = 0, col = 0;
        if (fread(&op, sizeof(op), 1, f) != 1 ||
            fread(&a,  sizeof(a),  1, f) != 1 ||
            fread(&b,  sizeof(b),  1, f) != 1 ||
            fread(&c,  sizeof(c),  1, f) != 1 ||
            fread(&line, sizeof(line), 1, f) != 1 ||
            fread(&col,  sizeof(col),  1, f) != 1) {
            goto truncated;
        }
        if (op < 0 || op >= BC_COUNT) {
            fprintf(stderr, "error: `%s` 第 %d 条指令的操作码非法（%d）\n", path, i, op);
            fclose(f);
            free(src_file);
            return false;
        }
        bc_emit(out, (OpCode)op, a, b, (SrcPos){ src_file, line, col });
        out->code[out->code_count - 1].c = (int16_t)c;
    }

    // ---- 一致性校验 ----
    // 函数原型里的 code_start/code_len 必须落在实际指令范围内。
    // 不检查的话，越界的 code_start 会让解释器跳到随机内存。
    for (int i = 0; i < out->func_count; i++) {
        FuncProto* fn = &out->funcs[i];
        if (fn->code_start < 0 || fn->code_start + fn->code_len > out->code_count) {
            fprintf(stderr,
                    "error: `%s` 中函数 `%s` 的代码范围越界（start=%d len=%d，共 %d 条指令）\n"
                    "  help: 字节码文件已损坏，请重新编译\n",
                    path, fn->name, fn->code_start, fn->code_len, out->code_count);
            fclose(f);
            free(src_file);
            return false;
        }
    }

    fclose(f);
    // src_file 的所有权转给 Bytecode——它的指针被 Insn.pos.file 引用，
    // 生命周期必须覆盖整个执行过程。刻意不释放。
    return true;

truncated:
    fprintf(stderr,
            "error: `%s` 文件不完整（%ld 字节），可能下载中断或被截断\n"
            "  help: 用 `x build <源文件>` 重新生成\n",
            path, fsize);
    fclose(f);
    return false;
}