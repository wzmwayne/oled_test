package com.wzmwayne.oledpanel

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

data class Book(val name: String, val size: Long)

class BookAdapter(
    private val onDelete: (Book) -> Unit
) : RecyclerView.Adapter<BookAdapter.VH>() {

    private val items = ArrayList<Book>()

    fun submit(newItems: List<Book>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_book, parent, false)
        return VH(view)
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: VH, position: Int) {
        holder.bind(items[position])
    }

    inner class VH(itemView: View) : RecyclerView.ViewHolder(itemView) {
        private val tvName: TextView = itemView.findViewById(R.id.tvBookName)
        private val tvSize: TextView = itemView.findViewById(R.id.tvBookSize)
        private val btnDel: TextView = itemView.findViewById(R.id.btnBookDelete)

        fun bind(book: Book) {
            tvName.text = book.name
            tvSize.text = formatSize(book.size)
            btnDel.setOnClickListener { onDelete(book) }
        }

        private fun formatSize(bytes: Long): String = when {
            bytes >= 1024 * 1024 -> String.format("%.1f MB", bytes / 1024f / 1024f)
            bytes >= 1024 -> String.format("%.1f KB", bytes / 1024f)
            else -> "$bytes B"
        }
    }
}
