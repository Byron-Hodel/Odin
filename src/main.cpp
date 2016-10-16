#include "common.cpp"
#include "profiler.cpp"
#include "unicode.cpp"
#include "tokenizer.cpp"
#include "parser.cpp"
#include "printer.cpp"
#include "checker/checker.cpp"
#include "codegen/codegen.cpp"

// NOTE(bill): `name` is used in debugging and profiling modes
i32 win32_exec_command_line_app(char *name, char *fmt, ...) {
	STARTUPINFOW start_info = {gb_size_of(STARTUPINFOW)};
	PROCESS_INFORMATION pi = {};
	char cmd_line[2048] = {};
	isize cmd_len;
	va_list va;
	gbTempArenaMemory tmp;
	String16 cmd;

	start_info.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	start_info.wShowWindow = SW_SHOW;
	start_info.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
	start_info.hStdOutput  = GetStdHandle(STD_OUTPUT_HANDLE);
	start_info.hStdError   = GetStdHandle(STD_ERROR_HANDLE);

	va_start(va, fmt);
	cmd_len = gb_snprintf_va(cmd_line, gb_size_of(cmd_line), fmt, va);
	va_end(va);
	// gb_printf("%.*s\n", cast(int)cmd_len, cmd_line);

	tmp = gb_temp_arena_memory_begin(&string_buffer_arena);
	defer (gb_temp_arena_memory_end(tmp));

	cmd = string_to_string16(string_buffer_allocator, make_string(cast(u8 *)cmd_line, cmd_len-1));

	if (CreateProcessW(NULL, cmd.text,
	                   NULL, NULL, true, 0, NULL, NULL,
	                   &start_info, &pi)) {
		DWORD exit_code = 0;

		WaitForSingleObject(pi.hProcess, INFINITE);
		GetExitCodeProcess(pi.hProcess, &exit_code);

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);

		return cast(i32)exit_code;
	} else {
		// NOTE(bill): failed to create process
		gb_printf_err("Failed to execute command:\n\t%s\n", cmd_line);
		return -1;
	}
}



enum ArchKind {
	ArchKind_x64,
	ArchKind_x86,
};

struct ArchData {
	BaseTypeSizes sizes;
	String llc_flags;
	String link_flags;
};

ArchData make_arch_data(ArchKind kind) {
	ArchData data = {};

	switch (kind) {
	case ArchKind_x64:
	default:
		data.sizes.word_size = 8;
		data.sizes.max_align = 16;
		data.llc_flags = make_string("-march=x86-64 ");
		data.link_flags = make_string("/machine:x64 ");
		break;

	case ArchKind_x86:
		data.sizes.word_size = 4;
		data.sizes.max_align = 8;
		data.llc_flags = make_string("-march=x86 ");
		data.link_flags = make_string("/machine:x86 ");
		break;
	}

	return data;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		gb_printf_err("using: %s [run] <filename> \n", argv[0]);
		return 1;
	}
	prof_init();

#if 1
	init_string_buffer_memory();
	init_global_error_collector();


	String module_dir = get_module_dir();

	init_universal_scope();

	char *init_filename = argv[1];
	b32 run_output = false;
	if (gb_strncmp(argv[1], "run", 3) == 0) {
		run_output = true;
		init_filename = argv[2];
	}
	Parser parser = {0};


	if (!init_parser(&parser)) {
		return 1;
	}
	// defer (destroy_parser(&parser));

	if (parse_files(&parser, init_filename) != ParseFile_None) {
		return 1;
	}


#if 1
	Checker checker = {};
	ArchData arch_data = make_arch_data(ArchKind_x64);

	init_checker(&checker, &parser, arch_data.sizes);
	// defer (destroy_checker(&checker));

	check_parsed_files(&checker);


#endif
#if 1
	ssaGen ssa = {};
	if (!ssa_gen_init(&ssa, &checker)) {
		return 1;
	}
	// defer (ssa_gen_destroy(&ssa));

	ssa_gen_tree(&ssa);

	// TODO(bill): Speedup writing to file for IR code
	ssa_gen_ir(&ssa);

	prof_print_all();

#if 1

	char const *output_name = ssa.output_file.filename;
	isize base_name_len = gb_path_extension(output_name)-1 - output_name;
	String output = make_string(cast(u8 *)output_name, base_name_len);

	int optimization_level = 0;
	optimization_level = gb_clamp(optimization_level, 0, 3);

	i32 exit_code = 0;
	// For more passes arguments: http://llvm.org/docs/Passes.html
	exit_code = win32_exec_command_line_app("llvm-opt",
		"%.*sbin/opt %s -o %.*s.bc "
		"-mem2reg "
		"-memcpyopt "
		"-die "
		// "-dse "
		// "-dce "
		// "-S "
		"",
		LIT(module_dir),
		output_name, LIT(output));
	if (exit_code != 0) {
		return exit_code;
	}

	#if 1
	// For more arguments: http://llvm.org/docs/CommandGuide/llc.html
	exit_code = win32_exec_command_line_app("llvm-llc",
		"%.*sbin/llc %.*s.bc -filetype=obj -O%d "
		"%.*s "
		// "-debug-pass=Arguments "
		"",
		LIT(module_dir),
		LIT(output),
		optimization_level,
		LIT(arch_data.llc_flags));
	if (exit_code != 0) {
		return exit_code;
	}


	gbString lib_str = gb_string_make(gb_heap_allocator(), "Kernel32.lib");
	// defer (gb_string_free(lib_str));
	char lib_str_buf[1024] = {};
	for_array(i, parser.system_libraries) {
		String lib = parser.system_libraries[i];
		isize len = gb_snprintf(lib_str_buf, gb_size_of(lib_str_buf),
		                        " %.*s.lib", LIT(lib));
		lib_str = gb_string_appendc(lib_str, lib_str_buf);
	}
	exit_code = win32_exec_command_line_app("msvc-link",
		"link %.*s.obj -OUT:%.*s.exe %s "
		"/defaultlib:libcmt "
		"/nologo /incremental:no /opt:ref /subsystem:console "
		"%.*s "
		"",
		LIT(output), LIT(output),
		lib_str, LIT(arch_data.link_flags));
	if (exit_code != 0) {
		return exit_code;
	}
	// prof_print_all();

	if (run_output) {
		win32_exec_command_line_app("odin run", "%.*s.exe", cast(int)base_name_len, output_name);
	}
	#endif
#endif
#endif
#endif


	return 0;
}
