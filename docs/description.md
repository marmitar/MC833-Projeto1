# Project 1 – Movie Streaming System Using TCP

**Course:** Computer Networks Programming (MC833) – UNICAMP

## 1. Introduction

This project consists of implementing a movie streaming query system based on a client-server architecture using TCP sockets. The server will store and manage a movie database, supporting registration, query, and removal operations. Communication between client and server will be done via TCP, ensuring reliable and lossless transmission.

Databases may be used for storage, but their use must be transparent to the end user; the system must behave the same whether a database or file-based storage is used.

## 2. Project Requirements

### 2.1 Protocol and Architecture

- **Protocol:** TCP (Transmission Control Protocol)
- **Model:** Client-Server
- **Server:** Concurrent (supports multiple simultaneous clients)
- **Clients:** Applications that send requests and receive responses

### 2.2 Movie Database

The server will store information about the movies, each with the following attributes:

| Field            | Description                          |
|------------------|--------------------------------------|
| Identifier       | Unique number for each movie         |
| Title            | Movie name                           |
| Genre            | One or more genres                   |
| Director         | Director's name                      |
| Release Year     | Year the movie was released          |

Storage options:

- Databases (e.g., SQLite, PostgreSQL, MySQL)
- Local files (e.g., JSON, CSV, binary)

Regardless of the storage method, the client must not notice any difference.

## 3. Features

### 3.1 Registration and Modification (Write Operations)

#### **(1)** Register a new movie

User provides movie data. The system generates a unique identifier and stores it.

#### **(2)** Add a new genre to an existing movie

Verifies existence and appends a genre.

#### **(3)** Remove a movie by its identifier

Deletes a movie if its identifier exists.

### 3.2 Queries (Read Operations)

#### **(4)** List all movie titles with their identifiers

Displays a list of movie IDs and titles.

#### **(5)** List information of all movies

Shows title, genre(s), director, and release year for all entries.

#### **(6)** List details of a specific movie

Searches by ID and returns all movie details.

#### **(7)** List all movies of a specific genre

Filters and lists movies belonging to the given genre.

## 4. Technical Requirements

### 4.1 Client and Server on Different Machines

- Must be executed on separate physical machines.
- Must be tested over a real network.

### 4.2 Server Concurrency

- Must support multiple concurrent clients.
- Use multiprocessing (fork) or multithreading (threads).
- Must use synchronization mechanisms (mutexes, semaphores, etc.) for concurrent data access.
- Must avoid race conditions and data corruption.

### 4.3 Communication Reliability

- TCP ensures ordered and complete message delivery.
- Implementation must avoid delays and communication losses.

### 4.4 Data Storage Options

- Option 1: Database (SQLite, PostgreSQL, MySQL)
- Option 2: Local Files (JSON, CSV, binary)

Client behavior must be identical in either case.

## 5. Deliverables

### 5.1 Source Code

- Implemented in C
- Must compile without errors on **Linux**
- Must include explanatory comments for each function

### 5.2 Report

The report must include:

- *Introduction:* General overview of the system
- *Architecture:* Diagram of the client-server model
- *Storage Structure:* How data is organized
- *Feature Description:* Detailed explanation of all features
- *Implementation Details:* Libraries, communication design
- *Use Cases*
- *Conclusion*
- *References*
