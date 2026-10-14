#!/usr/bin/env bash
# build 名审查测试：只运行 CMake 配置阶段（很快），使用临时构建目录，不影响 build/
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "$ROOT/plugins/student/__name_test_plugin.cpp"' EXIT
FAIL=0
expect() {  # expect <ok|fail> <名称> <说明>
  local want="$1" name="$2" desc="$3"
  rm -rf "$TMP/b" && mkdir -p "$TMP/b"
  if (cd "$TMP/b" && cmake "$ROOT" -DHGCSIM_BUILD_NAME="$name" > "$TMP/log" 2>&1); then got=ok; else got=fail; fi
  if [ "$got" = "$want" ]; then
    echo "[ OK ] $desc"
  else
    echo "[FAIL] $desc（期望 $want，实际 $got）"; tail -5 "$TMP/log"; FAIL=1
  fi
}
built_name() { sed -n 's/.*"name": "\(.*\)".*/\1/p' "$TMP/b/build_info.json" | head -1; }

RESERVED="$(cd "$TMP" && mkdir -p r && cd r && cmake "$ROOT" -DHGCSIM_BUILD_NAME= > /dev/null 2>&1; sed -n 's/.*"reserved_names": "\(.*\)".*/\1/p' build_info.json)"
echo "保留名：$RESERVED"
expect ok "" "原始源码不指定名称"
echo "       原始 build 名 = $(built_name)"
for r in ${RESERVED//,/ }; do
  expect fail "$r" "拒绝保留名 $r"
  expect fail "$(echo "$r" | tr '[:lower:]' '[:upper:]')" "拒绝保留名 $r（大写）"
done
expect ok "my_build-1.0" "接受合法名称"
expect fail "bad name" "拒绝含空格的名称"
expect fail "-x" "拒绝以符号开头的名称"
expect fail "$(printf 'a%.0s' $(seq 1 41))" "拒绝超过 40 个字符的名称"
# 添加自定义插件后：必须指定名称
cp "$ROOT/plugins/example_scheduler.cpp" "$ROOT/plugins/student/__name_test_plugin.cpp"
expect fail "" "添加插件后不指定名称被拒绝"
expect ok "with_plugin" "添加插件后指定名称"
rm -f "$ROOT/plugins/student/__name_test_plugin.cpp"
[ "$FAIL" = 0 ] && echo "build 名审查测试：全部通过" || { echo "build 名审查测试：存在失败"; exit 1; }
