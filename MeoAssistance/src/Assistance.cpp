#include "Assistance.h"

#include "WinMacro.h"
#include "Configer.h"
#include "Identify.h"
#include "Logger.hpp"
#include "AsstAux.h"

#include <time.h>
#include <filesystem>

using namespace asst;

Assistance::Assistance()
{
	DebugTraceFunction;

	m_configer.reload(GetResourceDir() + "config.json");

	m_pIder = std::make_shared<Identify>();
	for (auto&& [name, info] : m_configer.m_tasks)
	{
		m_pIder->add_image(name, GetResourceDir() + info.filename);
	}
	m_pIder->set_use_cache(m_configer.m_options.identify_cache);

	m_working_thread = std::thread(working_proc, this);
}

Assistance::~Assistance()
{
	DebugTraceFunction;

	if (m_pWindow != nullptr) {
		m_pWindow->showWindow();
	}

	m_thread_exit = true;
	m_thread_running = false;
	m_condvar.notify_one();

	if (m_working_thread.joinable()) {
		m_working_thread.join();
	}
}

std::optional<std::string> Assistance::set_emulator(const std::string& emulator_name)
{
	DebugTraceFunction;

	stop();

	auto create_handles = [&](const EmulatorInfo& info) -> bool {
		m_pWindow = std::make_shared<WinMacro>(info, HandleType::Window);
		m_pView = std::make_shared<WinMacro>(info, HandleType::View);
		m_pCtrl = std::make_shared<WinMacro>(info, HandleType::Control);
		return m_pWindow->captured() && m_pView->captured() && m_pCtrl->captured();
	};

	bool ret = false;
	std::string cor_name = emulator_name;

	std::unique_lock<std::mutex> lock(m_mutex);

	// 自动匹配模拟器，逐个找
	if (emulator_name.empty()) {
		for (auto&& [name, info] : m_configer.m_handles)
		{
			ret = create_handles(info);
			if (ret) {
				cor_name = name;
				break;
			}
		}
	}
	else {
		ret = create_handles(m_configer.m_handles[emulator_name]);
	}
	if (ret && m_pWindow->showWindow() && m_pWindow->resizeWindow()) {
		m_inited = true;
		return cor_name;
	}
	else {
		m_inited = false;
		return std::nullopt;
	}
}

void Assistance::start(const std::string& task)
{
	DebugTraceFunction;
	DebugTrace("Start |", task);


	if (m_thread_running || !m_inited) {
		return;
	}

	std::unique_lock<std::mutex> lock(m_mutex);
	m_configer.clear_exec_times();

	m_pIder->clear_cache();
	m_next_tasks.clear();
	m_next_tasks.emplace_back(task);
	m_thread_running = true;
	m_condvar.notify_one();
}

void Assistance::stop(bool block)
{
	DebugTraceFunction;
	DebugTrace("Stop |", block ? "block" : "non block");

	std::unique_lock<std::mutex> lock;
	if (block) { // 外部调用
		lock = std::unique_lock<std::mutex>(m_mutex);
		m_configer.clear_exec_times();
	}
	m_thread_running = false;
	m_next_tasks.clear();
	m_pIder->clear_cache();
}

bool Assistance::set_param(const std::string& type, const std::string& param, const std::string& value)
{
	DebugTraceFunction;
	DebugTrace("SetParam |", type, param, value);

	std::unique_lock<std::mutex> lock(m_mutex);
	return m_configer.set_param(type, param, value);
}

std::optional<std::string> Assistance::get_param(const std::string& type, const std::string& param)
{
	// DebugTraceFunction;

	std::unique_lock<std::mutex> lock(m_mutex);
	return m_configer.get_param(type, param);
}

bool asst::Assistance::print_window(const std::string& filename, bool block)
{
	DebugTraceFunction;
	DebugTrace("print_window |", block ? "block" : "non block");

	std::unique_lock<std::mutex> lock;
	if (block) { // 外部调用
		lock = std::unique_lock<std::mutex>(m_mutex);
	}

	auto cur_image = m_pView->getImage(m_pView->getWindowRect());
	if (cur_image.empty() || cur_image.cols < m_configer.DefaultWindowWidth || cur_image.rows < m_configer.DefaultWindowHeight) {
		DebugTraceError("Window image error");
		return false;
	}
	// 把模拟器边框的一圈裁剪掉，不然企鹅物流识别不出来
	auto&& window_info = m_pView->getEmulatorInfo();
	int x_offset = -window_info.x_offset + 5;
	int y_offset = -window_info.y_offset;// + 5;
	int width = m_configer.DefaultWindowWidth - 5;
	int height = m_configer.DefaultWindowHeight - 5;
	cv::Mat resize_mat( cur_image, cv::Rect(x_offset, y_offset, width, height));
	bool ret = cv::imwrite(filename.c_str(), resize_mat);
	
	if (ret) {
		DebugTraceInfo("PrintWindow to", filename);
	}
	else {
		DebugTraceError("PrintWindow error", filename);
	}
	return ret;
}

void Assistance::working_proc(Assistance* pThis)
{
	DebugTraceFunction;

	while (!pThis->m_thread_exit) {
		std::unique_lock<std::mutex> lock(pThis->m_mutex);
		if (pThis->m_thread_running) {
			auto cur_image = pThis->m_pView->getImage(pThis->m_pView->getWindowRect());

			if (cur_image.empty()) {
				DebugTraceError("Unable to capture window image!!!");
				pThis->stop(false);
				continue;
			}
			if (cur_image.cols < pThis->m_configer.DefaultWindowWidth || cur_image.rows < pThis->m_configer.DefaultWindowHeight) {
				DebugTraceInfo("Window Could not be minimized!!!");
				pThis->m_pWindow->showWindow();
				pThis->m_condvar.wait_for(lock,
					std::chrono::milliseconds(pThis->m_configer.m_options.identify_delay),
					[&]() -> bool { return !pThis->m_thread_running; });
				continue;
			}

			std::string matched_task;
			Rect matched_rect;
			// 逐个匹配当前可能的图像
			for (auto&& task_name : pThis->m_next_tasks) {

				double threshold = pThis->m_configer.m_tasks[task_name].threshold;
				double cache_threshold = pThis->m_configer.m_tasks[task_name].cache_threshold;

				auto&& [algorithm, value, rect] = pThis->m_pIder->find_image(cur_image, task_name, threshold);
				DebugTrace(task_name, "Type:", algorithm, "Value:", value);
				if (algorithm == AlgorithmType::JustReturn ||
					(algorithm == AlgorithmType::MatchTemplate && value >= threshold)
					|| (algorithm == AlgorithmType::CompareHist && value >= cache_threshold)) {
					matched_task = task_name;
					matched_rect = rect;
					break;
				}
			}

			// 执行任务
			if (!matched_task.empty()) {
				auto&& task = pThis->m_configer.m_tasks[matched_task];
				DebugTraceInfo("***Matched***", matched_task, "Type:", task.type);
				// 前置固定延时
				if (task.pre_delay > 0) {
					DebugTrace("PreDelay", task.pre_delay);
					// std::this_thread::sleep_for(std::chrono::milliseconds(task.pre_delay));
					bool cv_ret = pThis->m_condvar.wait_for(lock, std::chrono::milliseconds(task.pre_delay),
						[&]() -> bool { return !pThis->m_thread_running; });
					if (cv_ret) { continue; }
				}

				if (task.max_times != INT_MAX) {
					DebugTrace("CurTimes:", task.exec_times, "MaxTimes:", task.max_times);
				}
				if (task.exec_times < task.max_times) {
					// 随机延时功能
					if ((task.type & TaskType::BasicClick)
						&& pThis->m_configer.m_options.control_delay_upper != 0) {
						static std::default_random_engine rand_engine(std::chrono::system_clock::now().time_since_epoch().count());
						static std::uniform_int_distribution<unsigned> rand_uni(pThis->m_configer.m_options.control_delay_lower, pThis->m_configer.m_options.control_delay_upper);
						int delay = rand_uni(rand_engine);
						DebugTraceInfo("Random Delay", delay, "ms");
						bool cv_ret = pThis->m_condvar.wait_for(lock, std::chrono::milliseconds(delay),
							[&]() -> bool { return !pThis->m_thread_running; });
						if (cv_ret) { continue; }
					}

					switch (task.type) {
					case TaskType::ClickRect:
						matched_rect = task.specific_area;
						[[fallthrough]];
					case TaskType::ClickSelf:
						pThis->m_pCtrl->click(matched_rect);
						break;
					case TaskType::ClickRand:
						pThis->m_pCtrl->click(pThis->m_pCtrl->getWindowRect());
						break;
					case TaskType::DoNothing:
						break;
					case TaskType::Stop:
						DebugTrace("TaskType is Stop");
						pThis->stop(false);
						continue;
						break;
					case TaskType::PrintWindow:
						if (pThis->m_configer.m_options.print_window) {
							// 每次到结算界面，掉落物品不是一次性出来的，有个动画，所以需要等一会再截图
							int print_delay = pThis->m_configer.m_options.print_window_delay;
							DebugTraceInfo("Ready to print window, delay", print_delay);
							pThis->m_condvar.wait_for(lock,
								std::chrono::milliseconds(print_delay),
								[&]() -> bool { return !pThis->m_thread_running; });

							std::string dirname = GetCurrentDir() + "screenshot\\";
							std::filesystem::create_directory(dirname);
							auto time_str = StringReplaceAll(StringReplaceAll(GetFormatTimeString(), " ", "_"), ":", "-");
							std::string filename = dirname + time_str + ".png";
							pThis->print_window(filename, false);
						}
						break;
					default:
						DebugTraceError("Unknown option type:", task.type);
						break;
					}
					++task.exec_times;

					// 减少其他任务的执行次数
					// 例如，进入吃理智药的界面了，相当于上一次点蓝色开始行动没生效
					// 所以要给蓝色开始行动的次数减一
					for (auto&& reduce : task.reduce_other_times) {
						--pThis->m_configer.m_tasks[reduce].exec_times;
						DebugTrace("Reduce exec times", reduce, pThis->m_configer.m_tasks[reduce].exec_times);
					}
					// 后置固定延时
					if (task.rear_delay > 0) {
						DebugTrace("RearDelay", task.rear_delay);
						// std::this_thread::sleep_for(std::chrono::milliseconds(task.rear_delay));
						auto cv_ret = pThis->m_condvar.wait_for(lock, std::chrono::milliseconds(task.rear_delay),
							[&]() -> bool { return !pThis->m_thread_running; });
						if (cv_ret) { continue; }
					}
					pThis->m_next_tasks = pThis->m_configer.m_tasks[matched_task].next;
				}
				else {
					DebugTraceInfo("Reached limit");
					pThis->m_next_tasks = pThis->m_configer.m_tasks[matched_task].exceeded_next;
				}

				// 单纯为了打印日志。。感觉可以优化下
				std::string nexts_str;
				for (auto&& name : pThis->m_next_tasks) {
					nexts_str += name + ",";
				}
				if (nexts_str.back() == ',') {
					nexts_str.pop_back();
				}
				DebugTrace("Next:", nexts_str);
			}

			pThis->m_condvar.wait_for(lock,
				std::chrono::milliseconds(pThis->m_configer.m_options.identify_delay),
				[&]() -> bool { return !pThis->m_thread_running; });
		}
		else {
			pThis->m_condvar.wait(lock);
		}
	}
}
