// FST-X Arrow stream, row-list batches (rows given: a filter's matches or a list of rows). MIT licence.
// Part of r_arrow.cpp: included once, inside its anonymous namespace, after the helpers it uses (array_init,
// fixed_prep, string_load, ...). Kept in its own file so that the range path (r_arrow.cpp) and this path can change
// apart. Not a header of its own: it has no include guard and compiles only inside r_arrow.cpp (and so in the fuzz
// target, which includes r_arrow.cpp).

// One batch of rows: file rows a + sel[i] for i in [0, n) (sel ascending, repeats allowed, sel[n - 1] = b - a - 1),
// all inside one row group. h is the batch's struct array with one child per column.
void rows_batch(Stream& S, ArrayHold* h, uint64_t a, uint64_t b, const uint64_t* sel, uint64_t n) {
  const uint64_t* sp = sel;
  const size_t nc = S.cols.size();
  const int parts = nc && nc < (size_t)S.threads ? (int)(((size_t)S.threads + nc - 1) / nc) : 1;
  std::vector<SrcPtr> loaded(nc);
  std::vector<std::unique_ptr<FixedCol>> fx(nc);
  struct Task {
    size_t j;
    uint64_t i0, i1;
  };
  std::vector<Task> tasks;
  for (size_t j = 0; j < nc; ++j) {
    const ColSpec& c = S.cols[j];
    if (c.phys == FSTX_STRING) {
      loaded[j] = string_load(S, c, a, b, S.threads);
      tasks.push_back({j, 0, n});
      continue;
    }
    fx[j].reset(new FixedCol());
    FixedCol& F = *fx[j];
    F.c = &c;
    F.out = h->kids[j];
    F.a = a;
    F.n = n;
    F.sel = sp;
    const std::vector<uint64_t> cut = fixed_prep(S, F, b, parts);
    for (size_t q = 0; q + 1 < cut.size(); ++q) tasks.push_back({j, cut[q], cut[q + 1]});
  }
  auto run = [&](const Task& t, int threads) {
    if (fx[t.j]) fixed_part(S, *fx[t.j], t.i0, t.i1, threads);
    else string_column(S.cols[t.j], std::move(loaded[t.j]), sp, n, h->kids[t.j]);
  };
  const size_t nt = tasks.size();
  const int k = (int)std::min<size_t>((size_t)S.threads, nt);
  if (k <= 1) {
    for (const Task& t : tasks) run(t, S.threads);
  } else {
    std::atomic<size_t> nx{0};
    std::vector<std::exception_ptr> ex((size_t)k);
    auto work = [&](int id) {
      try {
        for (size_t i; (i = nx.fetch_add(1)) < nt;) run(tasks[i], 1);
      } catch (...) {
        ex[(size_t)id] = std::current_exception();
        nx.store(nt);
      }
    };
    std::vector<std::thread> ts;
    try {
      for (int i = 1; i < k; ++i) ts.emplace_back(work, i);
    } catch (...) {  // no more threads: the ones started and this one share the work
    }
    work(0);
    for (std::thread& t : ts) t.join();
    for (std::exception_ptr& x : ex)
      if (x) std::rethrow_exception(x);
  }
  for (size_t j = 0; j < nc; ++j)
    if (fx[j]) h->kids[j]->null_count = fx[j]->nulls.load();
}
