//
// Created by consti10 on 25.08.22.
//

#ifndef HWDECTOGL__USER_INPUT_H_
#define HWDECTOGL__USER_INPUT_H_

#include <getopt.h>
#include <iostream>

static const char optstr[] = "?w:h:i";
static const struct option long_options[] = {
	{"width", required_argument, nullptr, 'w'},
	{"height", required_argument, nullptr, 'h'},
	{"input", required_argument, nullptr, 'i'},
	{nullptr, 0, nullptr, 0},
};

// User input
struct UserOptions {
  int width=1280;
  int height=720;
  std::string filename;
};

static UserOptions parse_run_parameters(int argc, char *argv[]){
  UserOptions ret{};
  int c;
  while ((c = getopt_long(argc, argv, optstr, long_options, NULL)) != -1) {
	const char *tmp_optarg = optarg;
	switch (c) {
	  case 'w':ret.width=std::atoi(tmp_optarg);
		break;
	  case 'h':ret.height=std::atoi(tmp_optarg);
		break;
	  case 'i':ret.filename=std::string(tmp_optarg);
		break;
	  case '?':
	  default:
		std::cout << "Usage: \n" <<
					"-w --width -h --height\n"
				  "-i --input input_filename\n";
		exit(1);
	}
  }
  return ret;
}

#endif //HWDECTOGL__USER_INPUT_H_
