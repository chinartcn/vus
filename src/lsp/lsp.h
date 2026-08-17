/*
 * lsp.h — VUS 语言服务器（LSP）入口
 *
 * 通过标准输入输出按 JSON-RPC（Content-Length 分帧）与编辑器通信。
 * 实现 initialize / textDocument/completion / workspace/executeCommand /
 * shutdown / exit / textDocument/hover。三层补全：普通前缀、`.:` 详细前缀、`..:` 命令前缀。
 */

#ifndef VUS_LSP_H
#define VUS_LSP_H

/* LSP 服务器主入口：从 stdin 读请求、往 stdout 写响应；当 stdin EOF 时优雅退出并返回 0 */
int vus_lsp_main(int argc, char **argv);

#endif /* VUS_LSP_H */