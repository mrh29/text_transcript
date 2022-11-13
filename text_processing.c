/*****************************************************************************************
 * text_processing.c
 *  This file outputs a transcript of text conversations between you and
 *  another person using Apple's message database and sqlite3
 *
 *  Copyright 2022, Michael Horton
 ****************************************************************************************/
#include <stdio.h>
#include <sqlite3.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <stdlib.h>


/*****************************************************************************************
 * Custom Defines
 *  The defines in this section can be changed to suit your needs
 ****************************************************************************************/

 /* The default DB to use. You may
  * also pass an alternative db
  * as the first argument to the program */
#define DATABASE_PATH ""

#define TRANSCRIPT_FILENAME "transcript.txt"

/* Can be an Apple ID email or phone number with country code:
 * e.g. foo@mydomain.com or +16789998212 */
#define CONTACT_INFO  ""

// Names to be logged in the transcript
#define YOUR_NAME     "Alice"
#define THEIR_NAME    "Bob"

// Range of years texts can fall in
#define FIRST_YEAR    (2000uLL)
#define LAST_YEAR     (2022uLL)

// Messages to search as exact text
static char* exact_messgages[] = {"Hi", "ETA?", "Goodnight", "Happy Birthday!"};

// Messages to search as a substr
static char* substr_messages[] = {":)", "🤣", "lol"};

// Uses an array as a heap, so HEAP_LEVELS n makes 2^n - 1 entries in the heap
#define HEAP_LEVELS (4)

/*****************************************************************************************
 * Program Defines
 *  These defines are used by the program and should not need to be edited
 ****************************************************************************************/

#define COUNT_STRING "SELECT COUNT(*) FROM chat JOIN \
                            chat_message_join ON chat. \"ROWID\" = \
                            chat_message_join.chat_id JOIN message ON \
                            chat_message_join.message_id = \
                            message. \"ROWID\" WHERE chat.chat_identifier \
                            = \"%s\" AND message.text %s \"%s%s%s\";"

// Number of years to span
#define NUM_YEARS     (LAST_YEAR - FIRST_YEAR + 1)

// Months define
#define NUM_MONTHS    (12)

#define DAY_SECONDS   (60 * 60 * 24)
#define HOUR_SECONDS  (60 * 60)

// Format string for dates: YYYY-MM-DD HH:MM:SS 
#define DATETIME_FORMAT "%lu-%u-%u %u:%u:%u"

// Counters
static uint64_t sent_msg_count;
static uint64_t received_msg_count;
static uint64_t morning_msg_count;
static uint64_t afternoon_msg_count;
static uint64_t evening_msg_count;
static uint64_t night_msg_count;

static size_t total_message_length;

// Counts in different time periods
static uint64_t year_counts[NUM_YEARS];
static uint64_t month_counts[NUM_MONTHS];

// Size of message count arrays
#define EXACT_MESSAGES_COUNT   (sizeof(exact_messgages) / sizeof(char*))
#define SUBSTR_MESSAGES_COUNT  (sizeof(substr_messages) / sizeof(char*))

// Custom type for tracking gaps of time
typedef struct {
    uint64_t seconds;
    struct tm time_1;
    struct tm time_2;
} time_diff;

// Type for match type with message counts
 typedef enum {
    EXACT_MATCH,
    SUBSTR_MATCH
 } match_type_t;

// Heap definitions
#define HEAP_SIZE   ((1 << HEAP_LEVELS) - 1)

static uint64_t heap_elements = 0;
static time_diff longest_droughts[HEAP_SIZE];

/*****************************************************************************************
 * Function Declarations
 *  Forward declarations of functions used throughout the program
 ****************************************************************************************/

// Helper functions
static int count_messages(const char* msg_text, match_type_t match, FILE* f, sqlite3* db, char* errMsg);
static void datetime_to_count(char* datetime);
static const char* index_to_month(int i);

// Callback functions
static int catchall_callback(void *NotUsed, int argc, char **argv, char **azColName);
static int count_callback(void* f, int argc, char **argv, char **azColName);
static int message_history(void* f, int argc, char **argv, char **azColName);

// Heap functions
void insert(time_diff x, time_diff heap[], uint64_t* heap_size);
void heapify(time_diff heap[], uint64_t heap_size, uint64_t start);


int main(int argc, char* argv[]) {
    sqlite3* db;
    char *zErrMsg = 0;
    sent_msg_count = 0;
    received_msg_count = 0;
    morning_msg_count = 0;
    afternoon_msg_count = 0;
    evening_msg_count = 0;
    night_msg_count = 0;

    int rc;
    // if provided use the db path
    if (argc == 2) {
        rc = sqlite3_open(argv[1], &db);
    } else {
        rc = sqlite3_open(DATABASE_PATH, &db);
    }

    if (0 != rc) {
        printf("Error opening db\n");
    }

    FILE* f = fopen(TRANSCRIPT_FILENAME, "w");
    if (NULL == f) {
        printf("Error opening transcript.txt\n");
        sqlite3_close(db);
    }

    char buffer[1000];

    // Output a log of texts and count when they were sent/received
    snprintf(buffer, 1000, 
                "SELECT datetime(message_date/1000000000 + \
                    strftime('%%s','2001-01-01'), \
                    'unixepoch', 'localtime') \
                    as date_utc, text, \
                    is_from_me, LENGTH(text) FROM chat JOIN \
                    chat_message_join ON chat. \"ROWID\" = \
                    chat_message_join.chat_id JOIN message ON \
                    chat_message_join.message_id = \
                    message. \"ROWID\" WHERE chat.chat_identifier \
                    = \"%s\" AND message.associated_message_type = 0 \
                    AND message.text IS NOT NULL \
                    ORDER BY message_date;", CONTACT_INFO);

    rc = sqlite3_exec(db, buffer, message_history, f, &zErrMsg);
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        sqlite3_close(db);
        return 1;
    }


    uint64_t total_msg_count = sent_msg_count + received_msg_count;
    fprintf(f, "\nMsg Counts:\n");
    fprintf(f, "Total: %llu\n", total_msg_count);
    fprintf(f, "Sent: %llu\n", sent_msg_count);
    fprintf(f, "Received: %llu\n", received_msg_count);

    // Count Reactions
    snprintf(buffer, 1000, "SELECT COUNT(*) FROM chat JOIN \
                            chat_message_join ON chat. \"ROWID\" = \
                            chat_message_join.chat_id JOIN message ON \
                            chat_message_join.message_id = \
                            message. \"ROWID\" WHERE chat.chat_identifier \
                            = \"%s\" AND message.associated_message_type \
                            <> 0;", CONTACT_INFO);
    fprintf(f, "Reactions: ");
    rc = sqlite3_exec(db, buffer, count_callback, f, &zErrMsg);
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        sqlite3_close(db);
        return 1;
    }

    // Count Drawn Messages
    snprintf(buffer, 1000, "SELECT COUNT(*) FROM chat JOIN \
                            chat_message_join ON chat. \"ROWID\" = \
                            chat_message_join.chat_id JOIN message ON \
                            chat_message_join.message_id = \
                            message. \"ROWID\" WHERE chat.chat_identifier \
                            = \"%s\" AND message.text \
                            IS NULL;", CONTACT_INFO);
    fprintf(f, "Drawn: ");
    rc = sqlite3_exec(db, buffer, count_callback, f, &zErrMsg);
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        sqlite3_close(db);
        return 1;
    }

    // Count Expressive Messages
    snprintf(buffer, 1000, "SELECT COUNT(*) FROM chat JOIN \
                            chat_message_join ON chat. \"ROWID\" = \
                            chat_message_join.chat_id JOIN message ON \
                            chat_message_join.message_id = \
                            message. \"ROWID\" WHERE chat.chat_identifier \
                            = \"%s\" AND \
                            message.expressive_send_style_id \
                            IS NOT NULL;", CONTACT_INFO);
    fprintf(f, "Expressive: ");
    rc = sqlite3_exec(db, buffer, count_callback, f, &zErrMsg);
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        sqlite3_close(db);
        return 1;
    }


    fprintf(f, "Exact Counts:\n");
    for (int i = 0; i < EXACT_MESSAGES_COUNT; i++) {
        fprintf(f, "%s: ", exact_messgages[i]);
        rc = count_messages(exact_messgages[i], 
                                EXACT_MATCH, 
                                f, db, zErrMsg);
        if (rc != SQLITE_OK) {
            printf("SQL error: %s\n", zErrMsg);
            sqlite3_free(zErrMsg);
            sqlite3_close(db);
            return 1;
        }
    }

    fprintf(f, "\nSubstr Counts:\n");
    for (int i = 0; i < SUBSTR_MESSAGES_COUNT; i++) {
        fprintf(f, "%s (substr): ", substr_messages[i]);
        rc = count_messages(substr_messages[i], 
                                SUBSTR_MATCH, 
                                f, db, zErrMsg);
        if (rc != SQLITE_OK) {
            printf("SQL error: %s\n", zErrMsg);
            sqlite3_free(zErrMsg);
            sqlite3_close(db);
            return 1;
        }
    }

    fprintf(f, "Avg Msg Length: %f\n\n", 
                (double) total_message_length / (double) total_msg_count);

    fprintf(f, "Msg times:\n");
    fprintf(f, "Morning:   %llu\n", morning_msg_count);
    fprintf(f, "Afternoon: %llu\n", afternoon_msg_count);
    fprintf(f, "Evening:   %llu\n", evening_msg_count);
    fprintf(f, "Night:     %llu\n", night_msg_count);

    fprintf(f, "\nYears:\n");

    for (int i = 0; i < NUM_YEARS; i++) {
        fprintf(f, "%llu: %llu\n", FIRST_YEAR + i, year_counts[i]);
    }

    fprintf(f, "\nMonths:\n");

    uint64_t spring_count = 0;
    uint64_t summer_count = 0;
    uint64_t fall_count   = 0;
    uint64_t winter_count = 0;

    for (int i = 0; i < NUM_MONTHS; i++) {
        fprintf(f, "%s: %llu\n", index_to_month(i + 1), month_counts[i]);
        if (i + 1 > 2 && i + 1 < 6) {
            spring_count += month_counts[i];
        } else if (i + 1 > 5 && i + 1 < 9) {
            summer_count += month_counts[i];
        } else if (i + 1 > 8 && i + 1 < 12) {
            fall_count += month_counts[i];
        } else {
            winter_count += month_counts[i];
        }
    }

    fprintf(f, "\nSeasons:\n");
    fprintf(f, "Spring: %llu\n", spring_count);
    fprintf(f, "Summer: %llu\n", summer_count);
    fprintf(f, "Fall:   %llu\n", fall_count);
    fprintf(f, "Winter: %llu\n\n", winter_count);

    char time_1_buf[26];
    char time_2_buf[26];

    uint64_t days = 0;
    uint64_t hours = 0;
    uint64_t minutes = 0;
    uint64_t seconds = 0;

    for (int i = 0; i < HEAP_SIZE; i++) {
        days    = longest_droughts[i].seconds / DAY_SECONDS;
        hours   = (longest_droughts[i].seconds / HOUR_SECONDS) % 24;
        minutes = (longest_droughts[i].seconds / 60) % 60;
        seconds = longest_droughts[i].seconds % 60;
        fprintf(f, "%llu days, %llu hours, %llu minutes, %llu seconds\n%s%s\n", 
                    days, hours, minutes, seconds, 
                    asctime_r(&longest_droughts[i].time_1, time_1_buf), 
                    asctime_r(&longest_droughts[i].time_2, time_2_buf));
    }

    sqlite3_close(db);
    fclose(f);
    return 0;
}

/*****************************************************************************************
 * Callbacks
 *  Callbacks passed to the sqlite3 library for handling rows from a query
 ****************************************************************************************/
// Generic callback for SQL queries
static int catchall_callback(void *f, int argc, char **argv, char **azColName) {
    for (int i = 0; i < argc; i++) {
        fprintf(f, "%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
    }
    fprintf(f, "\n");
    return 0;
}

// Callback for counts that prints the description with the count
static int count_callback(void* f, int argc, char **argv, char **azColName) {
    fprintf(f, "%s\n", argv[0]);
    return 0;
}

// Callback used to parse entire message history
static int message_history(void *f, int argc, char **argv, char **azColName) {
    // update counts based on the datetime
    datetime_to_count(argv[0]);

    // Print the text
    if (strcmp(argv[2], "0") == 0) {
        fprintf(f, "%s %s: %s\n", argv[0], THEIR_NAME, argv[1]);
        received_msg_count++;
    } else {
        fprintf(f, "%s %s: %s\n", argv[0], YOUR_NAME, argv[1]);
        sent_msg_count++;
    }

    // Update message length
    total_message_length += atoi(argv[3]);
    return 0;
}

/*****************************************************************************************
 * Helpers
 *  Helpers for other functions
 ****************************************************************************************/
static const char* index_to_month(int i) {
    switch (i) {
        case 1:
            return "January";
        case 2:
            return "February";
        case 3:
            return "March";
        case 4:
            return "April";
        case 5:
            return "May";
        case 6:
            return "June";
        case 7:
            return "July";
        case 8:
            return "August";
        case 9:
            return "September";
        case 10:
            return "October";
        case 11:
            return "November";
        case 12:
            return "Decemeber";
        default:
            printf("Month out of range!\n");
            assert(0);
    }
}



// Given a datetime returned by a query, update counters
static void datetime_to_count(char* datetime) {
    static struct tm prev_time = {
        .tm_sec  = 0,
        .tm_min  = 0,
        .tm_hour = 0,
        .tm_mday = 0,
        .tm_mon  = 0,
        .tm_year = 0,
    };
    static uint8_t first_time = 0;

    unsigned int month, day, hour, minute, second;
    unsigned long year;
    int ret = sscanf(datetime, DATETIME_FORMAT, &year, &month, &day, &hour, &minute, &second);
    assert(ret == 6);

    if (year < FIRST_YEAR) {
        return;
    }

    struct tm timestamp = {
        .tm_sec = second,
        .tm_min = minute,
        .tm_hour = hour,
        .tm_mday = day,
        .tm_mon = month - 1,
        .tm_year = year -  1900,
    };

    if (!first_time) {
        first_time = 1;
        prev_time = timestamp;
    }


    // Get the time since last text
    time_t seconds = mktime(&timestamp);
    time_t prev_seconds = mktime(&prev_time);
    uint64_t diffSeconds = (uint64_t) difftime(seconds, prev_seconds);

    time_diff t = {
        .seconds = diffSeconds,
        .time_1  = prev_time,
        .time_2  = timestamp,
    };

    // Insert this differential to the heap
    insert(t, longest_droughts, &heap_elements);

    prev_time = timestamp;


    // Update counters
    ++month_counts[month - 1];

    ++year_counts[year - FIRST_YEAR];
    
    if (hour >= 6 && hour < 12) {
        morning_msg_count++;
    } else if (hour >= 12 && hour < 18) {
        afternoon_msg_count++;
    } else if (hour >= 18 && hour < 22) {
        evening_msg_count++;
    } else {
        night_msg_count++;
    }
}

// Function used to count texts that are exactly or contain msg_text
static int count_messages(const char* msg_text, match_type_t match, FILE* f, sqlite3* db, char* errMsg) {
    int rc;
    char buffer[500];
    char* format_str = match == SUBSTR_MATCH ? "%%" : "";
    snprintf(buffer, 500, COUNT_STRING, CONTACT_INFO, match == SUBSTR_MATCH ? "LIKE" : "=", format_str, msg_text, format_str);
    rc = sqlite3_exec(db, buffer, count_callback, (void*) f, &errMsg);
    return rc;
}

/*****************************************************************************************
 * Heap Module
 *  This implements a basic heap using an array
 ****************************************************************************************/
void heapify(time_diff heap[], uint64_t heap_size, uint64_t start) {
    if (1 != heap_size) {
        int largest_index = start;
        uint64_t left  = (2 * start) + 1;
        uint64_t right = (2 * start) + 2;

        if (left < heap_size && heap[left].seconds > heap[largest_index].seconds) {
            largest_index = left;
        }

        if (right < heap_size && heap[right].seconds > heap[largest_index].seconds) {
            largest_index = right;
        }

        if (largest_index != start) {
            time_diff tmp;
            tmp = heap[start];
            heap[start] = heap[largest_index];
            heap[largest_index] = tmp;
            heapify(heap, heap_size, largest_index);
        }
    }
}

void insert(time_diff x, time_diff heap[], uint64_t* heap_size) {
    if (*heap_size == 0) {
        heap[0] = x;
        *heap_size += 1;
    } else {
        if (*heap_size == HEAP_SIZE) {
            // Heap is full, need to check minimum
            int min_index = HEAP_SIZE / 2;
            int min_value = heap[min_index].seconds;

            // loop over potential minimums
            for (int i = *heap_size / 2 + 1; i < *heap_size; i++) {
                if (heap[i].seconds < min_value) {
                    min_value = heap[i].seconds;
                    min_index = i;
                }
            }

            if (x.seconds > min_value) {
                heap[min_index] = x;
                for (int i = min_index  / 2 - 1; i >= 0; i--) {
                    heapify(heap, *heap_size, i);
                }
            }

        } else {
            heap[*heap_size] = x;
            *heap_size += 1;
            for (int i = *heap_size / 2 - 1; i >= 0; i--) {
                heapify(heap, *heap_size, i);
            }

        }
    }
}
