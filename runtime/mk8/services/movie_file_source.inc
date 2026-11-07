// MK8 uses Extractor::SetDataSource(path) at slot 2; slot 15 also takes a DataSource.
static void mk8_ex_set_file_source(Cpu *c) {
    c->x[2] = c->x[1];
    ex_set_data_source_l(c);
}
