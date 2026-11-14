#pragma once
// DirectXMath annotations have no runtime meaning on the wasm target.
#define _In_
#define _Out_
#define _Out_opt_
#define _In_reads_(n)
#define _In_reads_bytes_(n)
#define _Out_writes_(n)
#define _Out_writes_bytes_(n)
#define _Success_(condition)
#define _Use_decl_annotations_
#define _Analysis_assume_(condition)
