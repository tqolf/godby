#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

class Recorder {
  public:
	struct Record {
		uint64_t sequence;
		uint64_t timestamp;
		uint32_t checkcode;
	};
	using Records = std::vector<Record>;

	Recorder(size_t capacity = 8192)
	{
		if (capacity > 0) { M_records.reserve(capacity); }
	}

	void Add(uint64_t sequence, uint64_t timestamp, uint32_t checkcode)
	{
		M_records.emplace_back(sequence, timestamp, checkcode);
	}

	void Save(const std::string &name)
	{
		if (!M_exported) {
			std::string filename = name + ".bin";
			std::ofstream ofs(filename, std::ios::binary);
			for (const auto &record : M_records) { ofs.write(reinterpret_cast<const char *>(&record), sizeof(Record)); }
			ofs.close();
			M_exported = true;
			M_records.clear();
		}
	}

	static Records Load(const std::string &filename)
	{
		Record record;
		Records records;
		std::ifstream ifs(filename, std::ios::binary);
		while (ifs.read(reinterpret_cast<char *>(&record), sizeof(Record))) { records.push_back(record); }
		return records;
	}

	static void Analyze(const std::string &output_file, const Records &sender_records, const Records &receiver_records, uint64_t time_tolerance = 1000)
	{
		std::ofstream ofs(output_file);
		size_t receiver_index = 0;
		for (const auto &s_record : sender_records) {
			auto best_match = receiver_records.end();
			int64_t best_time_diff = time_tolerance + 1;
			size_t search_start = receiver_index;

			// Iterate through receiver records and prioritize checkcode matching, then consider time tolerance and sequence
			for (size_t i = search_start; i < receiver_records.size(); ++i) {
				const auto &r_record = receiver_records[i];

				// Prioritize checkcode matching
				if (r_record.checkcode == s_record.checkcode) {
					int64_t time_diff = std::abs(static_cast<int64_t>(r_record.timestamp) - static_cast<int64_t>(s_record.timestamp));
					if (time_diff <= static_cast<int64_t>(time_tolerance) && time_diff < best_time_diff) {
						best_match = receiver_records.begin() + i;
						best_time_diff = time_diff;
						receiver_index = i + 1; // Update receiver index for next search
					}
				}
			}

			// Write best match or mark as exception
			if (best_match == receiver_records.end()) {
				ofs << s_record.sequence << "," << "<Error>" << "\n";
			} else {
				int64_t time_diff = static_cast<int64_t>(best_match->timestamp) - static_cast<int64_t>(s_record.timestamp);
				ofs << s_record.sequence << "," << time_diff << "\n";
			}
		}
	}

	static void Analyze(const std::string &output_file, const std::string &sender_file, const std::string &receiver_file, uint64_t time_tolerance = 1000)
	{
		auto sender_records = Load(sender_file);
		auto receiver_records = Load(receiver_file);
		Analyze(output_file, sender_records, receiver_records, time_tolerance);
	}

	static void Analyze(const std::string &output_file, const Recorder &sender, const Recorder &receiver, uint64_t time_tolerance = 1000)
	{
		Analyze(output_file, sender.M_records, receiver.M_records, time_tolerance);
	}

  protected:
	bool M_exported;
	Records M_records;
	std::string M_unexpected_value;
};

#include <random>
#include <chrono>
#include <iostream>
#include <crc32c/crc32c.h> // Using crc32c library to calculate checksum

template <typename Clock = std::chrono::high_resolution_clock>
uint64_t Microseconds()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

int main()
{
	std::mt19937 rng(std::random_device{}());
	std::uniform_real_distribution<> prob_dist(0.0, 1.0);

	// Test Case 1: Completely normal, sequence and checksum match
	{
		Recorder sender, receiver;
		for (uint64_t i = 0; i < 5000; ++i) {
			uint64_t timestamp = Microseconds();
			uint32_t checksum = crc32c::Crc32c(reinterpret_cast<const char *>(&i), sizeof(i));
			sender.Add(i, timestamp, checksum);
			receiver.Add(i, timestamp + rand() % 500, checksum);
		}
		Recorder::Analyze("output_normal.csv", sender, receiver);
	}

	// Test Case 2: Data can fully match, but there is a fixed sequence offset
	{
		Recorder sender, receiver;
		for (uint64_t i = 0; i < 5000; ++i) {
			uint64_t timestamp = Microseconds();
			uint32_t checksum = crc32c::Crc32c(reinterpret_cast<const char *>(&i), sizeof(i));
			sender.Add(i, timestamp, checksum);
			receiver.Add(i + 5, timestamp + rand() % 500, checksum); // Fixed offset of 5
		}
		Recorder::Analyze("output_offset.csv", sender, receiver);
	}

	// Test Case 3: Random packet loss with a certain probability
	{
		Recorder sender, receiver;
		for (uint64_t i = 0; i < 5000; ++i) {
			uint64_t timestamp = Microseconds();
			uint32_t checksum = crc32c::Crc32c(reinterpret_cast<const char *>(&i), sizeof(i));
			sender.Add(i, timestamp, checksum);
			if (prob_dist(rng) > 0.02) { // 2% probability of packet loss
				receiver.Add(i, timestamp + rand() % 500, checksum);
			}
		}
		Recorder::Analyze("output_loss.csv", sender, receiver);
	}

	// Test Case 4: Random checksum mismatch
	{
		Recorder sender, receiver;
		for (uint64_t i = 0; i < 5000; ++i) {
			uint64_t timestamp = Microseconds();
			uint32_t checksum = crc32c::Crc32c(reinterpret_cast<const char *>(&i), sizeof(i));
			sender.Add(i, timestamp, checksum);

			if (prob_dist(rng) > 0.01) { // 1% probability of checksum mismatch
				uint32_t modified_checksum = (prob_dist(rng) > 0.9) ? checksum + 1 : checksum;
				receiver.Add(i, timestamp + rand() % 500, modified_checksum);
			}
		}
		Recorder::Analyze("output_checksum_mismatch.csv", sender, receiver);
	}

	// Test Case 5: Random packet loss and checksum mismatch
	{
		Recorder sender, receiver;
		for (uint64_t i = 0; i < 5000; ++i) {
			uint64_t timestamp = Microseconds();
			uint32_t checksum = crc32c::Crc32c(reinterpret_cast<const char *>(&i), sizeof(i));
			sender.Add(i, timestamp, checksum);
			if (prob_dist(rng) > 0.02) {													   // 2% probability of packet loss
				uint32_t modified_checksum = (prob_dist(rng) > 0.9) ? checksum + 1 : checksum; // 10% probability of checksum mismatch
				receiver.Add(i, timestamp + rand() % 500, modified_checksum);
			}
		}
		Recorder::Analyze("output_loss_checksum_mismatch.csv", sender, receiver);
	}

	return 0;
}
